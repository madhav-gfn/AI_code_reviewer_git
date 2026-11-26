# Server deployment plan: Docker on Render Free

This plan targets a first prototype on Render's Free Web Service tier. It covers deployment readiness for the existing inference service, not the quiz feature.

## Important model limitation

The project README recommends Qwen2.5-Coder 1.5B Q4, whose model file is roughly 1 GB. Render Free provides 512 MB RAM and 0.1 CPU, so it cannot run that model with llama.cpp and its runtime memory. The free tier also sleeps after 15 minutes without traffic and may restart. It is suitable for validating a lightweight API/container, but **not for hosting this project's current model inference**.

For the Free-tier prototype, choose one of these explicit modes:

- Keep inference local through the existing daemon; use Render only for lightweight API/protocol experiments.
- Use a separately hosted inference backend, if one is selected and configured.
- Use a stub inference backend to validate service/client integration. This does not provide real code reviews.

Do not deploy a public endpoint that appears to offer real inference if it is using a stub. Model weights, prompts, and persistent session data must not depend on Render's ephemeral filesystem.

References: [Render compute plans](https://render.com/docs/compute-plans), [Render free service limits](https://render.com/docs/free), [Render web services](https://render.com/docs/web-services), [Docker on Render](https://render.com/docs/docker), [Render health checks](https://render.com/docs/health-checks), [Qwen model files](https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/tree/main).

## Phase 1 — Define the prototype boundary

**Work**

- Confirm which of the three inference modes above the prototype will use.
- Restrict access to trusted testers and set request-size, timeout, and usage limits.
- Document that cold starts are expected and that local files are not durable.

**Done when**

- The deployed service's role is clear to users, including whether it performs real inference.
- The model is not included or downloaded into the Free instance.

## Phase 2 — Add Render-aware server configuration

**Work**

- Keep the existing local daemon bound to loopback by default.
- Add a hosted API mode that binds to `0.0.0.0` and the `PORT` environment variable (Render defaults to `10000`).
- Validate configuration at startup and expose clear errors for missing or malformed settings.
- Disable remote `/shutdown` in hosted mode.

**Done when**

- Local mode remains localhost-only.
- Render can detect the hosted process listening on the configured port.

## Phase 3 — Secure and bound the HTTP API

**Work**

- Add authentication for hosted routes, even though Render provides HTTPS.
- Validate JSON and content types; enforce request size, timeout, and rate limits.
- Bound concurrency and return consistent errors for invalid, overloaded, or unavailable inference backends.
- Avoid logging source prompts, answers, credentials, or secret-bearing headers.
- Do not enable CORS unless a browser client requires it.

**Done when**

- Unauthorized, malformed, oversized, slow, and excessive requests are rejected safely.
- Hosted clients cannot access shutdown or other administrative controls.

## Phase 4 — Add health and lifecycle behavior

**Work**

- Add a liveness endpoint and a readiness endpoint. Readiness should indicate whether the selected backend is actually available; a stub should identify itself as a stub.
- Start the HTTP listener promptly and handle SIGTERM with graceful shutdown.
- Log service startup, request IDs, status, and latency without logging request bodies.
- Configure Render's HTTP health check path to match the intended readiness behavior.

**Done when**

- Render health checks distinguish a working API from an unavailable inference backend.
- Restarts and shutdowns do not leave invalid session state or misleading readiness.

## Phase 5 — Package the lightweight Docker service

**Work**

- Add a reproducible multi-stage Dockerfile using the project's CMake and vcpkg build.
- Build a small Linux image with only runtime dependencies; run as a non-root user.
- Add `.dockerignore` for build output, local model files, credentials, logs, and `.git` data.
- Do not download model weights during image build or startup on the Free plan.
- Add `render.yaml` for the Docker web service, health check, and non-secret environment configuration, or document equivalent Dashboard settings.

**Done when**

- Render builds the image from a clean repository checkout.
- The container starts within the platform's startup window and stays within Free instance memory.

## Phase 6 — Add hosted client transport

**Work**

- Preserve local daemon auto-start as the local backend.
- Add an explicit hosted URL setting and a secure token source for Render requests.
- Use HTTPS, bounded retries, request IDs, and clear handling for cold-start delays, auth errors, timeouts, and unavailable inference.
- Do not silently fall back between hosted and local backends.

**Done when**

- A CLI can explicitly select local or hosted mode.
- Hosted credentials are not printed or stored in plaintext project configuration.

## Phase 7 — Deploy and validate the Render Free prototype

**Work**

- Create a Render Web Service from the repository's Dockerfile or Blueprint.
- Configure secrets, environment variables, and the HTTP health check in Render.
- Verify first deploy, cold start after sleep, auth, malformed/oversized requests, backend-unavailable behavior, restart, and rollback.
- Confirm no important data is written only to the container filesystem.

**Done when**

- A trusted client can reach the service over HTTPS and gets the documented response for its configured inference mode.
- The service behaves correctly after sleep/restart and stays within the Free usage limits.

## Upgrade path for real hosted inference

When moving from prototype to hosted model inference, first measure peak RAM and latency with the selected model. Then move inference to a compute plan or separate provider with enough memory for model weights, KV cache, runtime, and concurrent requests. Render Free's 512 MB RAM and 0.1 CPU cannot host the README's recommended 1.5B Q4 model. Keep the API and inference worker boundary so only the worker needs the larger compute allocation.

## Current code gaps

The prototype implementation now provides hosted bearer authentication, Render `PORT` binding, an API-only stub mode, liveness/readiness endpoints, bounded request bodies and concurrency, basic rate limiting, a Docker image, a Render Blueprint, and explicit hosted CLI transport (`MYGIT_BACKEND=hosted`, `MYGIT_API_URL`, and `MYGIT_API_TOKEN`).

The hosted service supports only the stub backend today. It refuses to start unless `MYGIT_INFERENCE_BACKEND=stub`, labels stub responses, and does not load or download model weights. A real remote inference adapter, proxy-aware per-client rate limits, and deployment validation remain future work. Set `MYGIT_API_TOKEN` as a Render secret with at least 24 characters. The local daemon remains loopback-only and retains its local model behavior.
