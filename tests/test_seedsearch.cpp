#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "SeedSearch.h"
#include "sl/InstrumentGen.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cmath>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE("compareInstruments reproduces the demo page's scores") {
    // Exact agreement, not approximate. A "92% match" in the plugin has to be
    // the same 92% the demo page reports, or the number means nothing.
    std::ifstream in(std::string(SL_VECTORS_DIR) + "/search-scores.json");
    REQUIRE(in.good());
    nlohmann::json v;
    in >> v;
    REQUIRE(v["count"].get<int>() >= 300);

    int checked = 0;
    double worst = 0.0;
    for (const auto& p : v["pairs"]) {
        const auto a = static_cast<uint32_t>(p["a"].get<int64_t>());
        const auto b = static_cast<uint32_t>(p["b"].get<int64_t>());
        const double want = p["score"].get<double>();
        const double got = sl::compareInstruments(sl::generateInstrument(a),
                                                  sl::generateInstrument(b));
        INFO("seeds " << a << " vs " << b << ": want " << want << " got " << got);
        REQUIRE_THAT(got, WithinAbs(want, 1e-9));
        worst = std::max(worst, std::abs(got - want));
        ++checked;
    }
    INFO("worst absolute score difference " << worst);
    REQUIRE(checked >= 300);
}

TEST_CASE("an instrument matches itself and beats a different one") {
    const auto a = sl::generateInstrument(3703184240u);
    const auto b = sl::generateInstrument(13u);
    const double self = sl::compareInstruments(a, a);
    const double other = sl::compareInstruments(a, b);
    INFO("self " << self << " other " << other);
    REQUIRE(self > 99.9);
    REQUIRE(self <= 100.0001);
    REQUIRE(other < self);
}

TEST_CASE("search finds a better match than a random guess") {
    const auto target = sl::generateInstrument(3703184240u);

    std::atomic<int> batches{0};
    auto stop = [&] { return batches.fetch_add(1) >= 40; };

    const auto result = sl::SeedSearch::run(target, 12345u, 0.0, stop);
    REQUIRE(result.found);
    REQUIRE(result.tested >= 4000u);

    // Every candidate must be of the target's own instrument type: the last
    // digit of the seed selects it, and searching across types would waste
    // nearly all the effort.
    REQUIRE(result.seed % 10u == static_cast<uint32_t>(target.typeIndex));

    const double baseline = sl::compareInstruments(target, sl::generateInstrument(77u));
    INFO("best " << result.score << " after " << result.tested
         << " candidates, baseline " << baseline);
    REQUIRE(result.score > baseline);
}

TEST_CASE("search stops at the threshold and keeps its best when cancelled") {
    const auto target = sl::generateInstrument(1230u);

    // Threshold mode returns as soon as it is reached.
    std::atomic<int> n{0};
    auto never = [&] { return n.fetch_add(1) >= 100000; };
    const auto hit = sl::SeedSearch::run(target, 999u, 70.0, never);
    REQUIRE(hit.found);
    REQUIRE(hit.score >= 70.0);

    // Cancelling immediately still returns whatever was found in the first
    // batch, rather than discarding it -- zyn had a bug here once.
    std::atomic<int> m{0};
    auto immediately = [&] { return m.fetch_add(1) >= 1; };
    const auto cancelled = sl::SeedSearch::run(target, 555u, 0.0, immediately);
    REQUIRE(cancelled.found);
    REQUIRE(cancelled.tested == sl::SeedSearch::kBatchSize);
    REQUIRE(cancelled.score > 0.0);
}

TEST_CASE("search reports progress so a caller can audition the best so far") {
    const auto target = sl::generateInstrument(3703184240u);
    std::atomic<int> batches{0};
    int progressCalls = 0;
    double lastScore = -1.0;

    auto stop = [&] { return batches.fetch_add(1) >= 10; };
    sl::SeedSearch::run(target, 4242u, 0.0, stop, [&](const sl::SeedSearch::Result& r) {
        ++progressCalls;
        REQUIRE(r.score >= lastScore);   // best-so-far never goes backwards
        lastScore = r.score;
    });
    REQUIRE(progressCalls > 5);
}

#include <chrono>
#include <thread>

TEST_CASE("the async runner cancels promptly and keeps its best") {
    sl::SeedSearchRunner runner;
    const auto target = sl::generateInstrument(3703184240u);

    runner.start(target, 0.0);
    REQUIRE(runner.running());

    // Let it accumulate some progress, then cancel and confirm it stops.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto mid = runner.best();
    REQUIRE(mid.found);

    runner.cancel();
    for (int i = 0; i < 200 && runner.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE_FALSE(runner.running());

    const auto final = runner.best();
    REQUIRE(final.found);
    REQUIRE(final.score >= mid.score);   // cancelling keeps the best, never discards
}

TEST_CASE("starting a new search supersedes a running one") {
    sl::SeedSearchRunner runner;
    runner.start(sl::generateInstrument(3703184240u), 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Must not deadlock or leak the first thread.
    runner.start(sl::generateInstrument(13u), 0.0);
    REQUIRE(runner.running());
    runner.cancel();
    for (int i = 0; i < 200 && runner.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE_FALSE(runner.running());
}

TEST_CASE("restarting a search repeatedly leaves no thread behind") {
    // start() has to cancel and join whatever was running before it spawns
    // again. Getting that wrong leaks a thread per restart and lets an old
    // worker publish into the new search's results -- which in the plugin means
    // clicking Search twice quickly, or letting Find Similar interrupt itself.
    sl::SeedSearchRunner runner;
    const auto target = sl::generateInstrument(3703184240u);

    for (int i = 0; i < 40; ++i) {
        runner.start(target, 0.0);
        // Sometimes let it get going, sometimes cut it off immediately.
        if (i % 3 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        runner.cancel();
    }

    // Settle, then check the final state is coherent rather than a mixture of
    // several runs.
    for (int i = 0; i < 200 && runner.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE_FALSE(runner.running());

    const auto best = runner.best();
    const auto top = runner.top();
    if (best.found) {
        REQUIRE(best.score >= 0.0);
        REQUIRE(best.score <= 100.0);
    }
    for (size_t i = 1; i < top.size(); ++i)
        REQUIRE(top[i - 1].score >= top[i].score);
}

TEST_CASE("a fresh search reports its own results, not the last one's") {
    // Every candidate a search considers has the target's type digit as its
    // last, because the type is fixed by the seed's last digit and searching
    // across types would waste the effort. So anything reported with the wrong
    // digit came from a previous run.
    //
    // What this does NOT test, having been checked: removing either state reset
    // in start() leaves it passing. The publish callback overwrites the best
    // seed, score and top list unconditionally on its first progress report, a
    // few hundred microseconds in, so a stale value has no window in which to
    // be seen. Those resets are defensive, not load-bearing.
    //
    // What it does test is that a restart cannot leave a previous run's worker
    // publishing into the new results -- the type digit makes that visible --
    // which is the failure the join in start() actually prevents.
    sl::SeedSearchRunner runner;

    runner.start(sl::generateInstrument(3703184240u), 0.0);   // pad, digit 0
    for (int i = 0; i < 400 && runner.best().score < 90.0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    runner.cancel();
    for (int i = 0; i < 200 && runner.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const auto firstBest = runner.best();
    REQUIRE(firstBest.found);
    REQUIRE(firstBest.seed % 10u == 0u);

    runner.start(sl::generateInstrument(2360196101u), 0.0);   // bass, digit 1
    for (int i = 0; i < 200 && !runner.best().found; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto secondBest = runner.best();
    const auto secondTop = runner.top();
    runner.cancel();

    REQUIRE(secondBest.found);
    REQUIRE(secondBest.seed % 10u == 1u);
    for (const auto& e : secondTop) REQUIRE(e.seed % 10u == 1u);
}
