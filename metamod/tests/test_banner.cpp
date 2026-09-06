// The banner frame is computed, so a longer version string must not break it.
#include "core/banner.h"

#include "doctest.h"

#include <string>
#include <vector>

TEST_CASE("Banner is a rectangle whatever the version length") {
    for (const std::string& version : {std::string("v1.0.0"),
                                       std::string("v3.0.0-rc1+build.12345"),
                                       std::string("v0.0.0-dev")}) {
        const std::vector<std::string> lines =
            ch::BuildBanner(version, "Metamod:Source", 1);

        REQUIRE(lines.size() == 7);

        const size_t width = lines[0].size();
        for (const std::string& line : lines) {
            CHECK(line.size() == width);
        }

        CHECK(lines.front()[0] == '+');
        CHECK(lines.back()[0] == '+');
    }
}

TEST_CASE("Banner carries the version, platform and server number") {
    const std::vector<std::string> lines = ch::BuildBanner("v3.0.0", "Metamod:Source", 42);

    std::string all;
    for (const std::string& line : lines) all += line;

    CHECK(all.find("ConnectHistory v3.0.0") != std::string::npos);
    CHECK(all.find("Metamod:Source") != std::string::npos);
    CHECK(all.find("#42") != std::string::npos);
}
