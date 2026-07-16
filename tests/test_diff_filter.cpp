#include <catch2/catch_test_macros.hpp>

#include "ai/review_aggregator.h"
#include "diff_filter/diff_filter.h"
#include "git/git_diff.h"
#include "mygit/types.h"

using mygit::Issue;
using mygit::ReviewResult;
using mygit::Severity;
using mygit::ai::aggregate_reviews;
using mygit::diff_filter::DiffFilterConfig;
using mygit::diff_filter::filter;
using mygit::git::FileDiff;

namespace {

std::string text_file_chunk(const std::string& path, size_t approx_body_bytes = 0) {
    std::string chunk = "diff --git a/" + path + " b/" + path +
                         "\nindex 1111111..2222222 100644\n"
                         "--- a/" + path + "\n+++ b/" + path +
                         "\n@@ -1,1 +1,1 @@\n-old line\n+new line\n";
    while (chunk.size() < approx_body_bytes) {
        chunk += "+padding line to grow this hunk past the size limit\n";
    }
    return chunk;
}

std::string binary_file_chunk(const std::string& path) {
    return "diff --git a/" + path + " b/" + path +
           "\nindex 1111111..2222222 100644\nBinary files a/" + path + " and b/" + path +
           " differ\n";
}

}  // namespace

TEST_CASE("diff_filter passes a normal text diff through unchanged") {
    const std::string diff = text_file_chunk("main.cpp");
    const auto result = filter(diff);

    REQUIRE_FALSE(result.rejected);
    REQUIRE(result.skipped.empty());
    REQUIRE(result.patch_text == diff);
}

TEST_CASE("diff_filter skips files with binary extensions") {
    const std::string diff = text_file_chunk("main.cpp") + binary_file_chunk("logo.png");
    const auto result = filter(diff);

    REQUIRE_FALSE(result.rejected);
    REQUIRE(result.skipped.size() == 1);
    REQUIRE(result.skipped.front().find("logo.png") != std::string::npos);
    REQUIRE(result.patch_text.find("logo.png") == std::string::npos);
    REQUIRE(result.patch_text.find("main.cpp") != std::string::npos);
}

TEST_CASE("diff_filter skips individual files exceeding max_per_file_bytes") {
    DiffFilterConfig cfg;
    cfg.max_per_file_bytes = 200;

    const std::string small = text_file_chunk("small.cpp");
    const std::string huge = text_file_chunk("huge.cpp", /*approx_body_bytes=*/500);
    const auto result = filter(small + huge, cfg);

    REQUIRE_FALSE(result.rejected);
    REQUIRE(result.skipped.size() == 1);
    REQUIRE(result.skipped.front().find("huge.cpp") != std::string::npos);
    REQUIRE(result.patch_text == small);
}

TEST_CASE("diff_filter rejects when total kept size exceeds max_total_diff_bytes") {
    DiffFilterConfig cfg;
    cfg.max_total_diff_bytes = 100;

    const std::string diff = text_file_chunk("a.cpp") + text_file_chunk("b.cpp");
    const auto result = filter(diff, cfg);

    REQUIRE(result.rejected);
    REQUIRE_FALSE(result.reject_reason.empty());
}

TEST_CASE("diff_filter rejects when too many files changed") {
    DiffFilterConfig cfg;
    cfg.max_file_count = 2;

    const std::string diff =
        text_file_chunk("a.cpp") + text_file_chunk("b.cpp") + text_file_chunk("c.cpp");
    const auto result = filter(diff, cfg);

    REQUIRE(result.rejected);
    REQUIRE_FALSE(result.reject_reason.empty());
}

TEST_CASE("diff_filter FileDiff overload skips pure renames") {
    FileDiff renamed;
    renamed.old_path = "old_name.cpp";
    renamed.new_path = "new_name.cpp";
    renamed.patch = "diff --git a/old_name.cpp b/new_name.cpp\nsimilarity index 100%\n";
    renamed.is_rename = true;
    renamed.has_content_changes = false;

    const auto result = filter(std::vector<FileDiff>{renamed});

    REQUIRE_FALSE(result.rejected);
    REQUIRE(result.kept_files.empty());
    REQUIRE(result.skipped.size() == 1);
    REQUIRE(result.skipped.front().find("pure rename") != std::string::npos);
}

TEST_CASE("diff_filter FileDiff overload keeps renames with content changes") {
    FileDiff renamed;
    renamed.old_path = "old_name.cpp";
    renamed.new_path = "new_name.cpp";
    renamed.patch = "diff --git a/old_name.cpp b/new_name.cpp\n+actual change\n";
    renamed.is_rename = true;
    renamed.has_content_changes = true;

    const auto result = filter(std::vector<FileDiff>{renamed});

    REQUIRE_FALSE(result.rejected);
    REQUIRE(result.kept_files.size() == 1);
    REQUIRE(result.skipped.empty());
}

TEST_CASE("diff_filter FileDiff overload skips binaries") {
    FileDiff bin;
    bin.old_path = bin.new_path = "logo.png";
    bin.patch = "diff --git a/logo.png b/logo.png\nBinary files differ\n";
    bin.is_binary = true;

    const auto result = filter(std::vector<FileDiff>{bin});

    REQUIRE_FALSE(result.rejected);
    REQUIRE(result.kept_files.empty());
    REQUIRE(result.skipped.size() == 1);
    REQUIRE(result.skipped.front().find("binary") != std::string::npos);
}

TEST_CASE("diff_filter FileDiff overload keeps normal files") {
    FileDiff fd;
    fd.old_path = fd.new_path = "main.cpp";
    fd.patch = "diff --git a/main.cpp b/main.cpp\n+change\n";

    const auto result = filter(std::vector<FileDiff>{fd});

    REQUIRE_FALSE(result.rejected);
    REQUIRE(result.kept_files.size() == 1);
    REQUIRE(result.patch_text == fd.patch);
}

TEST_CASE("aggregate_reviews merges issues from all files") {
    ReviewResult a;
    a.safe = true;
    a.issues.push_back(Issue{Severity::Low, "a.cpp", 1, "minor"});

    ReviewResult b;
    b.safe = true;
    b.issues.push_back(Issue{Severity::Medium, "b.cpp", 2, "also minor"});

    const auto combined = aggregate_reviews({a, b});

    REQUIRE(combined.safe);
    REQUIRE(combined.issues.size() == 2);
}

TEST_CASE("aggregate_reviews is unsafe if any input is unsafe") {
    ReviewResult a;
    a.safe = true;

    ReviewResult b;
    b.safe = false;
    b.issues.push_back(Issue{Severity::Critical, "b.cpp", 2, "bad"});

    const auto combined = aggregate_reviews({a, b});

    REQUIRE_FALSE(combined.safe);
    REQUIRE(combined.issues.size() == 1);
}

TEST_CASE("aggregate_reviews on empty input is safe with no issues") {
    const auto combined = aggregate_reviews({});

    REQUIRE(combined.safe);
    REQUIRE(combined.issues.empty());
}
