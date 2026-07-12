#include "git/git_diff.h"

#include <git2.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace mygit::git {

std::string GitDiff::get_staged_diff() const {
    // Initialize libgit2 for the current process (RAII static ensures single init/shutdown)
    static struct Libgit2Init { Libgit2Init(){ git_libgit2_init(); } ~Libgit2Init(){ git_libgit2_shutdown(); } } libgit2init;

    git_repository* repo_raw = nullptr;
    int err = git_repository_open_ext(&repo_raw, nullptr, 0, nullptr);
    if (err != 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to open repository: ") + (e && e->message ? e->message : "unknown"));
    }
    std::unique_ptr<git_repository, void(*)(git_repository*)> repo(repo_raw, git_repository_free);

    // Load the index
    git_index* index_raw = nullptr;
    err = git_repository_index(&index_raw, repo.get());
    if (err != 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to read index: ") + (e && e->message ? e->message : "unknown"));
    }
    std::unique_ptr<git_index, void(*)(git_index*)> index(index_raw, git_index_free);

    // Try to get HEAD tree (may not exist for an empty repo)
    git_tree* head_tree_raw = nullptr;
    git_reference* head_ref_raw = nullptr;
    if (git_repository_head(&head_ref_raw, repo.get()) == 0) {
        std::unique_ptr<git_reference, void(*)(git_reference*)> head_ref(head_ref_raw, git_reference_free);
        git_object* peeled = nullptr;
        if (git_reference_peel(&peeled, head_ref.get(), GIT_OBJECT_COMMIT) == 0 && peeled != nullptr) {
            std::unique_ptr<git_object, void(*)(git_object*)> peeled_obj(peeled, git_object_free);
            git_commit* commit = reinterpret_cast<git_commit*>(peeled);
            if (git_commit_tree(&head_tree_raw, commit) != 0) {
                head_tree_raw = nullptr;
            }
        }
    }
    std::unique_ptr<git_tree, void(*)(git_tree*)> head_tree(head_tree_raw, [](git_tree* p){ if (p) git_tree_free(p); });

    // Diff between HEAD (tree) and index produces the "staged" patch
    git_diff* diff_raw = nullptr;
    err = git_diff_tree_to_index(&diff_raw, repo.get(), head_tree.get(), index.get(), nullptr);
    if (err != 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to create diff: ") + (e && e->message ? e->message : "unknown"));
    }
    std::unique_ptr<git_diff, void(*)(git_diff*)> diff(diff_raw, git_diff_free);

    // Convert diff to patch text
    git_buf buf = GIT_BUF_INIT;
    err = git_diff_to_buf(&buf, diff.get(), GIT_DIFF_FORMAT_PATCH);
    if (err != 0) {
        git_buf_dispose(&buf);
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("git: failed to convert diff to buffer: ") + (e && e->message ? e->message : "unknown"));
    }

    std::string result;
    if (buf.ptr && buf.size > 0) {
        result.assign(buf.ptr, buf.size);
    }
    git_buf_dispose(&buf);
    return result;
}

} // namespace mygit::git
