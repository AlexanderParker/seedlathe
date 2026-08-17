#include <catch2/catch_test_macros.hpp>
#include "sl/Version.h"
#include <string>

TEST_CASE("core links and reports its version") {
    REQUIRE(std::string(sl::kVersion) == "0.1.0");
}
