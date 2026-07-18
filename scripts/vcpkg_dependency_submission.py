#!/usr/bin/env python3
"""Submit vcpkg's resolved dependency graph to GitHub's Dependency Submission API.

GitHub's dependency graph does not natively parse vcpkg.json (see the ecosystem
table in docs.github.com/en/code-security/supported-coding-languages-and-frameworks).
This script builds an equivalent snapshot from two local, no-rebuild-required sources:

  1. The vcpkg.spdx.json files vcpkg already writes for every installed package
     under vcpkg_installed/vcpkg/pkgs/*/share/*/vcpkg.spdx.json (name, version,
     license, source location).
  2. `vcpkg depend-info` for the dependency edges between those packages.

and POSTs it to /repos/{owner}/{repo}/dependency-graph/snapshots.

Usage:
    python scripts/vcpkg_dependency_submission.py --dry-run
    GITHUB_TOKEN=ghp_xxx python scripts/vcpkg_dependency_submission.py
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.request
import urllib.error
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
    if result.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)}\n{result.stderr}")
    return result.stdout


def git_remote_repo():
    url = run(["git", "remote", "get-url", "origin"]).strip()
    url = url.removesuffix(".git")
    if url.startswith("git@github.com:"):
        return url.removeprefix("git@github.com:")
    if "github.com/" in url:
        return url.split("github.com/", 1)[1]
    raise RuntimeError(f"could not parse owner/repo from remote url: {url}")


def load_spdx_ports(build_dir: Path) -> dict:
    """Return {port_name: {version, download_location, license}}.

    Reads from the real installed tree (vcpkg_installed/<triplet>/share/*/vcpkg.spdx.json)
    rather than vcpkg's internal build-doc cache (vcpkg_installed/vcpkg/pkgs/), since the
    latter can be stale/missing for packages restored from an already-installed state.
    """
    ports = {}
    installed_dir = build_dir / "vcpkg_installed"
    if not installed_dir.is_dir():
        raise RuntimeError(f"no vcpkg_installed dir at {installed_dir} -- run a vcpkg install first")

    for spdx_file in sorted(installed_dir.glob("*/share/*/vcpkg.spdx.json")):
        data = json.loads(spdx_file.read_text(encoding="utf-8"))
        port_pkg = next(
            (p for p in data.get("packages", []) if p.get("SPDXID") == "SPDXRef-port"),
            None,
        )
        if port_pkg is None:
            continue

        name = port_pkg["name"]
        triplet = spdx_file.parent.parent.parent.name  # vcpkg_installed/<triplet>/share/<port>/...
        is_release = triplet.endswith("-release")

        existing = ports.get(name)
        if existing is not None and existing["_is_release"] and not is_release:
            continue  # keep the target-triplet (release) entry over the host one

        ports[name] = {
            "version": port_pkg.get("versionInfo", "0"),
            "download_location": port_pkg.get("downloadLocation", ""),
            "license": port_pkg.get("licenseConcluded", "NOASSERTION"),
            "_is_release": is_release,
        }
    return ports


def load_dependency_edges(direct_deps: list[str]) -> dict:
    """Return {port_name: [dependency_port_names]} via `vcpkg depend-info`."""
    vcpkg_root = os.environ.get("VCPKG_ROOT")
    if not vcpkg_root:
        raise RuntimeError("VCPKG_ROOT is not set")
    vcpkg_exe = Path(vcpkg_root) / ("vcpkg.exe" if os.name == "nt" else "vcpkg")

    out = subprocess.run(
        [str(vcpkg_exe), "depend-info", "--format=list",
         "--triplet=x64-windows-release", *direct_deps],
        capture_output=True, text=True, cwd=REPO_ROOT,
    )
    if out.returncode != 0:
        raise RuntimeError(f"vcpkg depend-info failed:\n{out.stderr}")

    edges = {}
    for line in out.stderr.splitlines():
        line = line.strip()
        if not line or ":" not in line:
            continue
        lhs, rhs = line.split(":", 1)
        name = lhs.split("[", 1)[0].split(":", 1)[0].strip()
        deps = [
            d.strip().split("[", 1)[0].split(":", 1)[0]
            for d in rhs.split(",") if d.strip()
        ]
        edges.setdefault(name, [])
        for d in deps:
            if d and d not in edges[name]:
                edges[name].append(d)
    return edges


def to_purl(name: str, info: dict) -> str:
    version = info["version"]
    dl = info["download_location"]
    if dl and dl not in ("NONE", "NOASSERTION") and "@" in dl:
        return f"pkg:generic/{name}@{version}?vcs_url={urllib.parse.quote(dl, safe='')}"
    return f"pkg:generic/{name}@{version}"


def build_snapshot(repo: str, sha: str, ref: str, ports: dict, edges: dict, direct: set) -> dict:
    resolved = {}
    for name, info in ports.items():
        deps = [d for d in edges.get(name, []) if d in ports and d != name]
        resolved[name] = {
            "package_url": to_purl(name, info),
            "relationship": "direct" if name in direct else "indirect",
            "scope": "runtime",
            "dependencies": deps,
        }

    now = datetime.now(timezone.utc).isoformat()
    return {
        "version": 0,
        "sha": sha,
        "ref": ref,
        "job": {
            "correlator": "vcpkg-manual-submission",
            "id": now,
        },
        "detector": {
            "name": "vcpkg-spdx-submission",
            "version": "1.0.0",
            "url": f"https://github.com/{repo}",
        },
        "scanned": now,
        "manifests": {
            "vcpkg.json": {
                "name": "vcpkg.json",
                "file": {"source_location": "vcpkg.json"},
                "resolved": resolved,
            }
        },
    }


def submit(repo: str, token: str, snapshot: dict):
    req = urllib.request.Request(
        url=f"https://api.github.com/repos/{repo}/dependency-graph/snapshots",
        data=json.dumps(snapshot).encode("utf-8"),
        method="POST",
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req) as resp:
            print(resp.status, resp.read().decode())
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode()}", file=sys.stderr)
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build/default")
    parser.add_argument("--repo", default=None, help="owner/repo, defaults to origin remote")
    parser.add_argument("--sha", default=None, help="defaults to current HEAD")
    parser.add_argument("--ref", default=None, help="defaults to refs/heads/<current branch>")
    parser.add_argument("--dry-run", action="store_true", help="print snapshot, don't submit")
    parser.add_argument("--out", default=None, help="also write snapshot json to this path")
    args = parser.parse_args()

    manifest = json.loads((REPO_ROOT / "vcpkg.json").read_text(encoding="utf-8"))
    direct = set(
        d["name"] if isinstance(d, dict) else d for d in manifest.get("dependencies", [])
    )

    build_dir = REPO_ROOT / args.build_dir
    ports = load_spdx_ports(build_dir)
    edges = load_dependency_edges(sorted(direct))

    repo = args.repo or git_remote_repo()
    sha = args.sha or run(["git", "rev-parse", "HEAD"]).strip()
    ref = args.ref or f"refs/heads/{run(['git', 'rev-parse', '--abbrev-ref', 'HEAD']).strip()}"

    snapshot = build_snapshot(repo, sha, ref, ports, edges, direct)

    print(f"Resolved {len(ports)} packages ({len(direct)} direct) for {repo}@{sha[:8]} ({ref})",
          file=sys.stderr)

    if args.out:
        Path(args.out).write_text(json.dumps(snapshot, indent=2), encoding="utf-8")
        print(f"Wrote snapshot to {args.out}", file=sys.stderr)

    if args.dry_run:
        print(json.dumps(snapshot, indent=2))
        return

    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        print("GITHUB_TOKEN is not set; re-run with --dry-run or export a token", file=sys.stderr)
        sys.exit(1)
    submit(repo, token, snapshot)


if __name__ == "__main__":
    main()
