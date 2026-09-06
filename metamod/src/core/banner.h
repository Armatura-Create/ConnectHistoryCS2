// Startup banner.
//
// The frame is computed, not hardcoded with spaces: the version comes from the
// release tag and may be longer than expected ("3.0.0-rc1"), and a broken frame
// in the server console is the first thing the owner sees about this plugin.
#pragma once

#include <string>
#include <vector>

namespace ch {

std::vector<std::string> BuildBanner(const std::string& version,
                                     const std::string& platform,
                                     int serverId);

}  // namespace ch
