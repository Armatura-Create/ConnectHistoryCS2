#include "core/jobs.h"

namespace ch {

std::string WriteJob::Describe() const {
    switch (kind) {
        case JobKind::ServerUpsert:
            return "server #" + std::to_string(serverId) + " (" + address + ")";
        case JobKind::SessionOpen:
            return "open " + std::to_string(steamId64) + " (" + nickname + ")";
        case JobKind::SessionClose:
            return "close " + std::to_string(steamId64) + " (" + nickname + ", " +
                   std::to_string(durationSeconds) + "s)";
        case JobKind::OnlineSnapshot:
            return "snapshot server #" + std::to_string(serverId) + ": " +
                   std::to_string(players) + " players";
    }
    return "unknown job";
}

}  // namespace ch
