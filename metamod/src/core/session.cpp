#include "core/session.h"

#include <algorithm>

namespace ch {

void OpenSession::AddPing(int32_t ping) {
    if (ping <= 0 || ping > 2000) return;

    if (_pingSamples == 0) {
        _pingMin = ping;
        _pingMax = ping;
    } else {
        if (ping < _pingMin) _pingMin = ping;
        if (ping > _pingMax) _pingMax = ping;
    }

    _pingSum += ping;
    ++_pingSamples;
}

void OpenSession::NoteTeam(int32_t team, int64_t now) {
    if (team == _lastTeam) return;

    AccrueTeamTime(now);

    if (_lastTeam >= 0) ++_teamChanges;
    _lastTeam = team;
}

void OpenSession::AccrueTeamTime(int64_t now) {
    if (_teamSince == 0) _teamSince = now;

    if (_lastTeam == kTeamSpectator || _lastTeam == kTeamUnassigned) {
        const int64_t elapsed = std::max<int64_t>(0, now - _teamSince);
        _spectatorSeconds += static_cast<int32_t>(elapsed);
    }

    _teamSince = now;
}

size_t SessionRegistry::Count() const {
    std::lock_guard<std::mutex> guard(_mutex);
    return _sessions.size();
}

void SessionRegistry::Add(OpenSession session) {
    std::lock_guard<std::mutex> guard(_mutex);
    _sessions[session.steamId64] = std::move(session);
}

bool SessionRegistry::TryGet(uint64_t steamId, OpenSession* out) const {
    std::lock_guard<std::mutex> guard(_mutex);
    auto it = _sessions.find(steamId);
    if (it == _sessions.end()) return false;

    if (out != nullptr) *out = it->second;
    return true;
}

bool SessionRegistry::Take(uint64_t steamId, OpenSession* out) {
    std::lock_guard<std::mutex> guard(_mutex);
    auto it = _sessions.find(steamId);
    if (it == _sessions.end()) return false;

    if (out != nullptr) *out = std::move(it->second);
    _sessions.erase(it);
    return true;
}

std::vector<OpenSession> SessionRegistry::TakeAll() {
    std::lock_guard<std::mutex> guard(_mutex);

    std::vector<OpenSession> all;
    all.reserve(_sessions.size());
    for (auto& pair : _sessions) all.push_back(std::move(pair.second));
    _sessions.clear();
    return all;
}

}  // namespace ch
