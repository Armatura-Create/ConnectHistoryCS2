#include "core/banner.h"

#include <algorithm>

namespace ch {
namespace {

const char* const kMark[] = {
    " ####   #   #",
    "#       #   #",
    "#       #####",
    "#       #   #",
    " ####   #   #",
};

constexpr size_t kMarkRows = sizeof(kMark) / sizeof(kMark[0]);

}  // namespace

std::vector<std::string> BuildBanner(const std::string& version,
                                     const std::string& platform, int serverId) {
    std::vector<std::string> info = {
        "ConnectHistory " + version,
        "Connection history and player analytics",
        std::string(),
        "Platform : " + platform,
        "Server   : #" + std::to_string(serverId),
    };

    size_t width = 0;
    for (const std::string& line : info) width = std::max(width, line.size());

    info[2] = std::string(width, '-');

    const size_t markWidth = std::string(kMark[0]).size();
    const std::string border = "+" + std::string(2 + markWidth + 3 + width + 2, '-') + "+";

    std::vector<std::string> lines;
    lines.push_back(border);
    for (size_t i = 0; i < kMarkRows; ++i) {
        std::string padded = info[i];
        padded.resize(width, ' ');
        lines.push_back("|  " + std::string(kMark[i]) + "   " + padded + "  |");
    }
    lines.push_back(border);

    return lines;
}

}  // namespace ch
