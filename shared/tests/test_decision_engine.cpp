#include <catch2/catch_test_macros.hpp>

#include "decision_engine/decision_engine.h"
#include "mygit/types.h"

using mygit::Issue;
using mygit::ReviewResult;
using mygit::Severity;
using mygit::decision_engine::DecisionEngine;

TEST_CASE("DecisionEngine blocks when a critical issue is present") {
    DecisionEngine engine;
    ReviewResult result;
    result.safe = false;
    result.issues.push_back(Issue{Severity::Critical, "parser.cpp", 53, "Possible nullptr dereference"});

    REQUIRE_FALSE(engine.should_allow(result, /*force_ai=*/false));
}

TEST_CASE("DecisionEngine allows when there are no critical issues") {
    DecisionEngine engine;
    ReviewResult result;
    result.safe = true;
    result.issues.push_back(Issue{Severity::Low, "main.cpp", 10, "Minor style issue"});

    REQUIRE(engine.should_allow(result, /*force_ai=*/false));
}

TEST_CASE("DecisionEngine allows with no issues at all") {
    DecisionEngine engine;
    ReviewResult result;

    REQUIRE(engine.should_allow(result, /*force_ai=*/false));
}

TEST_CASE("force_ai overrides a critical block") {
    DecisionEngine engine;
    ReviewResult result;
    result.safe = false;
    result.issues.push_back(Issue{Severity::Critical, "parser.cpp", 53, "Possible nullptr dereference"});

    REQUIRE(engine.should_allow(result, /*force_ai=*/true));
}
