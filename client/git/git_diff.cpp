#include "git/git_diff.h"

#include <git2.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace mygit::git {

namespace {

// Initialize libgit2 for the current process (RAII static ensures single init/shutdown)
struct Libgit2Init {
    Libgit2Init() { git_libgit2_init(); }
    ~Libgit2Init() { git_libgit2_shutdown(); }
};

std::unique_ptr<git_repository, void (*)(git_repository*)> open_repo() {
    static Libgit2Init libgit2init;

    git_repository* repo_raw = nullptr;
    int err = git_repository_open_ext(&repo_raw, nullptr, GIT_REPOSITORY_OPEN_FROM_ENV, nullptr);
    if (err != 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to open repository: ") +
                                  (e && e->message ? e->message : "unknown"));
    }
    return {repo_raw, git_repository_free};
}

// Diffs HEAD's tree against the index (i.e. the staged changes), with
// rename/copy detection applied. Returns nullptr-owning unique_ptr wrapper.
std::unique_ptr<git_diff, void (*)(git_diff*)> create_staged_diff(git_repository* repo) {
    git_index* index_raw = nullptr;
    int err = git_repository_index(&index_raw, repo);
    if (err != 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to read index: ") +
                                  (e && e->message ? e->message : "unknown"));
    }
    std::unique_ptr<git_index, void (*)(git_index*)> index(index_raw, git_index_free);

    // Try to get HEAD tree (may not exist for an empty repo)
    git_tree* head_tree_raw = nullptr;
    git_reference* head_ref_raw = nullptr;
    if (git_repository_head(&head_ref_raw, repo) == 0) {
        std::unique_ptr<git_reference, void (*)(git_reference*)> head_ref(head_ref_raw,
                                                                            git_reference_free);
        git_object* peeled = nullptr;
        if (git_reference_peel(&peeled, head_ref.get(), GIT_OBJECT_COMMIT) == 0 &&
            peeled != nullptr) {
            std::unique_ptr<git_object, void (*)(git_object*)> peeled_obj(peeled,
                                                                           git_object_free);
            git_commit* commit = reinterpret_cast<git_commit*>(peeled);
            if (git_commit_tree(&head_tree_raw, commit) != 0) {
                head_tree_raw = nullptr;
            }
        }
    }
    std::unique_ptr<git_tree, void (*)(git_tree*)> head_tree(
        head_tree_raw, [](git_tree* p) { if (p) git_tree_free(p); });

    git_diff* diff_raw = nullptr;
    err = git_diff_tree_to_index(&diff_raw, repo, head_tree.get(), index.get(), nullptr);
    if (err != 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to create diff: ") +
                                  (e && e->message ? e->message : "unknown"));
    }
    std::unique_ptr<git_diff, void (*)(git_diff*)> diff(diff_raw, git_diff_free);

    // Detect renames/copies before the diff is turned into patch text so
    // moved-but-unchanged files show up as GIT_DELTA_RENAMED instead of a
    // full delete+add (bottleneck #4).
    git_diff_find_options find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
    find_opts.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES;
    find_opts.rename_threshold = 50;
    git_diff_find_similar(diff.get(), &find_opts);

    return diff;
}

}  // namespace

std::string GitDiff::get_staged_diff() const {
    auto repo = open_repo();
    auto diff = create_staged_diff(repo.get());

    git_buf buf = GIT_BUF_INIT;
    int err = git_diff_to_buf(&buf, diff.get(), GIT_DIFF_FORMAT_PATCH);
    if (err != 0) {
        git_buf_dispose(&buf);
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to convert diff to buffer: ") +
                                  (e && e->message ? e->message : "unknown"));
    }

    std::string result;
    if (buf.ptr && buf.size > 0) {
        result.assign(buf.ptr, buf.size);
    }
    git_buf_dispose(&buf);
    return result;
}

std::vector<FileDiff> GitDiff::get_staged_file_diffs() const {
    auto repo = open_repo();
    auto diff = create_staged_diff(repo.get());

    std::vector<FileDiff> results;
    const size_t n = git_diff_num_deltas(diff.get());
    results.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const git_diff_delta* delta = git_diff_get_delta(diff.get(), i);
        if (delta == nullptr) {
            continue;
        }

        FileDiff fd;
        fd.old_path = delta->old_file.path ? delta->old_file.path : "";
        fd.new_path = delta->new_file.path ? delta->new_file.path : "";
        fd.is_rename = (delta->status == GIT_DELTA_RENAMED);
        fd.is_binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
        fd.has_content_changes = !(fd.is_rename && delta->similarity == 100);

        git_patch* patch_raw = nullptr;
        if (git_patch_from_diff(&patch_raw, diff.get(), i) == 0 && patch_raw != nullptr) {
            std::unique_ptr<git_patch, void (*)(git_patch*)> patch(patch_raw, git_patch_free);
            git_buf buf = GIT_BUF_INIT;
            if (git_patch_to_buf(&buf, patch.get()) == 0 && buf.ptr && buf.size > 0) {
                fd.patch.assign(buf.ptr, buf.size);
            }
            git_buf_dispose(&buf);
        }

        results.push_back(std::move(fd));
    }

    return results;
}

}  // namespace mygit::git
