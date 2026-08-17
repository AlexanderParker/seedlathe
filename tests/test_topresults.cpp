#include <catch2/catch_test_macros.hpp>

#include "TopResults.h"

#include <thread>
#include <vector>

TEST_CASE("a top list keeps the best entries in order") {
    sl::TopList top;
    REQUIRE(top.offer(1u, 50.0));
    REQUIRE(top.offer(2u, 90.0));
    REQUIRE(top.offer(3u, 70.0));

    const auto& e = top.entries();
    REQUIRE(e.size() == 3);
    REQUIRE(e[0].seed == 2u);
    REQUIRE(e[1].seed == 3u);
    REQUIRE(e[2].seed == 1u);
}

TEST_CASE("a seed appears once, at its best score") {
    sl::TopList top;
    REQUIRE(top.offer(7u, 40.0));
    REQUIRE(top.offer(7u, 80.0));       // improved
    REQUIRE_FALSE(top.offer(7u, 60.0)); // worse than what is already kept

    REQUIRE(top.entries().size() == 1);
    REQUIRE(top.entries()[0].score == 80.0);
}

TEST_CASE("a top list is bounded and drops the worst") {
    sl::TopList top;
    for (uint32_t i = 0; i < sl::TopList::kCapacity + 10; ++i)
        top.offer(i, double(i));

    const auto& e = top.entries();
    REQUIRE(e.size() == sl::TopList::kCapacity);
    REQUIRE(e.front().score == double(sl::TopList::kCapacity + 9));
    REQUIRE(e.back().score == double(10));

    // Once full, something worse than the worst kept entry changes nothing --
    // this is the early-out that makes the list affordable at a million offers
    // a second.
    REQUIRE_FALSE(top.offer(9999u, 0.0));
    REQUIRE(e.size() == sl::TopList::kCapacity);

    // And something better does get in.
    REQUIRE(top.offer(9999u, 1000.0));
    REQUIRE(top.entries().front().seed == 9999u);
    REQUIRE(top.entries().size() == sl::TopList::kCapacity);
}

TEST_CASE("a top list stays ordered whatever order entries arrive in") {
    sl::TopList top;
    const double scores[] = {12, 99, 3, 55, 71, 44, 88, 1, 63, 27};
    uint32_t seed = 0;
    for (double s : scores) top.offer(seed++, s);

    const auto& e = top.entries();
    for (size_t i = 1; i < e.size(); ++i)
        REQUIRE(e[i - 1].score >= e[i].score);
    REQUIRE(e.front().score == 99.0);
}

TEST_CASE("a snapshot merges offers from several threads without losing any") {
    // The sample search runs a worker pool, and every worker offers into one
    // shared snapshot. Losing entries to a race would show up as a results list
    // that flickers or drops rows.
    sl::TopSnapshot snap;
    constexpr int kThreads = 4;
    constexpr int kPer = 200;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&snap, t] {
            for (int i = 0; i < kPer; ++i)
                snap.offer(static_cast<uint32_t>(t * kPer + i), double(t * kPer + i));
        });
    }
    for (auto& th : threads) th.join();

    const auto e = snap.read();
    REQUIRE(e.size() == sl::TopList::kCapacity);
    for (size_t i = 1; i < e.size(); ++i)
        REQUIRE(e[i - 1].score >= e[i].score);

    // The highest score offered by any thread must have survived.
    REQUIRE(e.front().score == double(kThreads * kPer - 1));
}
