#include "diff_filter/diff_filter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace mygit::diff_filter {

namespace {

const std::set<std::string>& binary_extensions() {
    static const std::set<std::string> exts = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".svg", ".pdf", ".zip",
        ".tar", ".gz",  ".exe",  ".dll", ".so",  ".dylib", ".o", ".obj", ".lib",
        ".a",   ".woff", ".woff2", ".ttf", ".eot", ".mp3", ".mp4", ".mov", ".avi", ".wav",
    };
    return exts;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Splits a unified diff into chunks, each starting at a "diff --git " line
// and running up to (but not including) the start of the next one.
std::vector<std::string> split_file_chunks(const std::string& raw_diff) {
    std::vector<std::string> chunks;
    size_t pos = raw_diff.find("diff --git ");
    if (pos == std::string::npos) {
        return chunks;
    }
    while (pos != std::string::npos) {
        size_t next = raw_diff.find("\ndiff --git ", pos);
        size_t chunk_end = (next == std::string::npos) ? raw_diff.size() : next + 1;
        chunks.push_back(raw_diff.substr(pos, chunk_end - pos));
        pos = (next == std::string::npos) ? std::string::npos : next + 1;
    }
    return chunks;
}

// Extracts the "a/<path>" portion of a "diff --git a/<path> b/<path>" header line.
std::string extract_path(const std::string& chunk) {
    static const std::string marker = "diff --git a/";
    size_t start = chunk.find(marker);
    if (start == std::string::npos) {
        return {};
    }
    start += marker.size();
    size_t sep = chunk.find(" b/", start);
    size_t eol = chunk.find('\n', start);
    size_t path_end = (sep != std::string::npos && (eol == std::string::npos || sep < eol))
                           ? sep
                           : (eol == std::string::npos ? chunk.size() : eol);
    return chunk.substr(start, path_end - start);
}

bool has_binary_marker(const std::string& chunk) {
    return chunk.find("Binary files ") != std::string::npos &&
           chunk.find(" differ") != std::string::npos;
}

bool has_binary_extension(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const std::string ext = to_lower(std::filesystem::path(path).extension().string());
    return binary_extensions().count(ext) > 0;
}

}  // namespace

FilteredDiff filter(const std::string& raw_diff, const DiffFilterConfig& cfg) {
    FilteredDiff result;
    if (raw_diff.empty()) {
        return result;
    }

    const std::vector<std::string> chunks = split_file_chunks(raw_diff);

    std::vector<std::string> kept_chunks;
    size_t kept_bytes = 0;
    for (const std::string& chunk : chunks) {
        const std::string path = extract_path(chunk);
        const std::string display = path.empty() ? "<unknown file>" : path;

        if (has_binary_marker(chunk) || has_binary_extension(path)) {
            result.skipped.push_back(display + " (binary)");
            continue;
        }
        if (chunk.size() > cfg.max_per_file_bytes) {
            result.skipped.push_back(display + " (too large: " + std::to_string(chunk.size()) +
                                      " bytes)");
            continue;
        }

        kept_chunks.push_back(chunk);
        kept_bytes += chunk.size();
    }

    const int file_count = static_cast<int>(chunks.size());
    if (file_count > cfg.max_file_count) {
        result.rejected = true;
        result.reject_reason = "Too many files changed (" + std::to_string(file_count) +
                                " > limit of " + std::to_string(cfg.max_file_count) + ")";
        return result;
    }
    if (kept_bytes > cfg.max_total_diff_bytes) {
        result.rejected = true;
        result.reject_reason = "Diff too large (" + std::to_string(kept_bytes) +
                                " bytes > limit of " + std::to_string(cfg.max_total_diff_bytes) +
                                " bytes)";
        return result;
    }

    for (const std::string& chunk : kept_chunks) {
        result.patch_text += chunk;
    }
    return result;
}

FilteredDiff filter(const std::vector<git::FileDiff>& file_diffs, const DiffFilterConfig& cfg) {
    FilteredDiff result;
    if (file_diffs.empty()) {
        return result;
    }

    std::vector<const git::FileDiff*> kept;
    size_t kept_bytes = 0;
    for (const git::FileDiff& fd : file_diffs) {
        const std::string display = fd.new_path.empty() ? fd.old_path : fd.new_path;

        if (fd.is_binary) {
            result.skipped.push_back(display + " (binary)");
            continue;
        }
        if (fd.is_rename && !fd.has_content_changes) {
            result.skipped.push_back(display + " (pure rename)");
            continue;
        }
        if (fd.patch.size() > cfg.max_per_file_bytes) {
            result.skipped.push_back(display + " (too large: " + std::to_string(fd.patch.size()) +
                                      " bytes)");
            continue;
        }

        kept.push_back(&fd);
        kept_bytes += fd.patch.size();
    }

    const int file_count = static_cast<int>(file_diffs.size());
    if (file_count > cfg.max_file_count) {
        result.rejected = true;
        result.reject_reason = "Too many files changed (" + std::to_string(file_count) +
                                " > limit of " + std::to_string(cfg.max_file_count) + ")";
        return result;
    }
    if (kept_bytes > cfg.max_total_diff_bytes) {
        result.rejected = true;
        result.reject_reason = "Diff too large (" + std::to_string(kept_bytes) +
                                " bytes > limit of " + std::to_string(cfg.max_total_diff_bytes) +
                                " bytes)";
        return result;
    }

    for (const git::FileDiff* fd : kept) {
        result.patch_text += fd->patch;
        result.kept_files.push_back(*fd);
    }
    return result;
}

}  // namespace mygit::diff_filter
