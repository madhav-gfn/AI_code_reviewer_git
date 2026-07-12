#include "git/git_status.h"

#include <git2.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace mygit::git {

static void ensure_libgit2_initialized() {
    static struct Init { Init(){ git_libgit2_init(); } ~Init(){ git_libgit2_shutdown(); } } init;
    (void)init;
}

std::string GitStatus::get_current_branch() const {
    ensure_libgit2_initialized();

    git_repository* repo_raw = nullptr;
    if (git_repository_open_ext(&repo_raw, nullptr, 0, nullptr) != 0) {
        return std::string();
    }
    std::unique_ptr<git_repository, void(*)(git_repository*)> repo(repo_raw, git_repository_free);

    git_reference* head_raw = nullptr;
    if (git_repository_head(&head_raw, repo.get()) != 0 || head_raw == nullptr) {
        return std::string();
    }
    std::unique_ptr<git_reference, void(*)(git_reference*)> head(head_raw, git_reference_free);

    const char* name = git_reference_shorthand(head.get());
    return name ? std::string(name) : std::string();
}

bool GitStatus::has_staged_changes() const {
    ensure_libgit2_initialized();

    git_repository* repo_raw = nullptr;
    if (git_repository_open_ext(&repo_raw, nullptr, 0, nullptr) != 0) {
        return false;
    }
    std::unique_ptr<git_repository, void(*)(git_repository*)> repo(repo_raw, git_repository_free);

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

    git_status_list* statuslist = nullptr;
    if (git_status_list_new(&statuslist, repo.get(), &opts) != 0 || statuslist == nullptr) {
        return false;
    }
    std::unique_ptr<git_status_list, void(*)(git_status_list*)> list(statuslist, git_status_list_free);

    size_t count = git_status_list_entrycount(statuslist);
    for (size_t i = 0; i < count; ++i) {
        const git_status_entry* entry = git_status_byindex(statuslist, i);
        if (!entry) continue;
        unsigned int s = entry->status;
        const unsigned int indexMask = GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED | GIT_STATUS_INDEX_TYPECHANGE;
        if (s & indexMask) return true;
    }
    return false;
}

}  // namespace mygit::git
