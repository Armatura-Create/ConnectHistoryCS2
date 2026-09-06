#include "core/spool.h"

#include "core/util/fs.h"

#include "core/logger.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace ch {
namespace {

using nlohmann::json;

void PutOptional(json* node, const char* key, bool present, const std::string& value) {
    if (present) (*node)[key] = value;
}

void ReadOptional(const json& node, const char* key, bool* present, std::string* value) {
    const auto it = node.find(key);
    if (it == node.end() || !it->is_string()) {
        *present = false;
        return;
    }
    *present = true;
    *value = it->get<std::string>();
}

// Чтение БЕЗ исключений — см. пояснение в config.cpp: игровая сборка идёт
// с -fno-exceptions, и там nlohmann/json на ошибке зовёт std::abort().
// Битая строка спула обязана быть пропущена, а не убить сервер.
bool TryGet(const json& value, bool* out) {
    if (!value.is_boolean()) return false;
    *out = value.get<bool>();
    return true;
}

bool TryGet(const json& value, int* out) {
    if (!value.is_number_integer()) return false;
    *out = static_cast<int>(value.get<int64_t>());
    return true;
}

bool TryGet(const json& value, int64_t* out) {
    if (!value.is_number_integer()) return false;
    *out = value.get<int64_t>();
    return true;
}

bool TryGet(const json& value, uint32_t* out) {
    if (!value.is_number_unsigned()) return false;
    const uint64_t raw = value.get<uint64_t>();
    if (raw > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(raw);
    return true;
}

bool TryGet(const json& value, uint64_t* out) {
    if (!value.is_number_unsigned()) return false;
    *out = value.get<uint64_t>();
    return true;
}

bool TryGet(const json& value, std::string* out) {
    if (!value.is_string()) return false;
    *out = value.get<std::string>();
    return true;
}

template <typename T>
T ReadOr(const json& node, const char* key, T fallback) {
    const auto it = node.find(key);
    if (it == node.end() || it->is_null()) return fallback;

    T value{};
    return TryGet(*it, &value) ? value : fallback;
}

// Спул содержит ники и IP игроков — файл не должен читаться кем попало
// на shared-хостинге.
void ProtectFile(const std::string& path) {
#ifndef _WIN32
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#else
    (void)path;
#endif
}

}  // namespace

std::string SerializeJob(const WriteJob& job) {
    json node;
    node["kind"] = static_cast<int>(job.kind);
    node["attempts"] = job.attempts;
    node["serverId"] = job.serverId;

    switch (job.kind) {
        case JobKind::ServerUpsert:
            node["address"] = job.address;
            node["hostname"] = job.hostname;
            node["seenAt"] = job.seenAt;
            break;

        case JobKind::OnlineSnapshot:
            node["takenAt"] = job.takenAt;
            node["players"] = job.players;
            node["bots"] = job.bots;
            node["maxPlayers"] = job.maxPlayers;
            node["map"] = job.map;
            break;

        case JobKind::SessionOpen:
        case JobKind::SessionClose:
            node["sessionKey"] = job.sessionKey;
            node["steamId64"] = job.steamId64;
            node["accountId"] = job.accountId;
            node["nickname"] = job.nickname;
            node["startedAt"] = job.startedAt;

            if (job.kind == JobKind::SessionOpen) {
                node["connectMap"] = job.connectMap;
                node["playersOnline"] = job.playersOnline;
                node["maxPlayers"] = job.maxPlayers;
                node["pluginVersion"] = job.pluginVersion;
                PutOptional(&node, "clientLang", job.hasClientLang, job.clientLang);
                PutOptional(&node, "playerIp", job.hasPlayerIp, job.playerIp);
                PutOptional(&node, "ipHash", job.hasIpHash, job.ipHash);
                PutOptional(&node, "ipSubnet", job.hasIpSubnet, job.ipSubnet);
                PutOptional(&node, "countryIso", job.hasCountryIso, job.countryIso);
                PutOptional(&node, "countryName", job.hasCountryName, job.countryName);
                PutOptional(&node, "city", job.hasCity, job.city);
            } else {
                node["endedAt"] = job.endedAt;
                node["durationSeconds"] = job.durationSeconds;
                node["spectatorSeconds"] = job.spectatorSeconds;
                node["countSpectatorTime"] = job.countSpectatorTime;
                node["disconnectMap"] = job.disconnectMap;
                node["disconnectReason"] = job.disconnectReason;
                node["disconnectReasonName"] = job.disconnectReasonName;
                node["endKind"] = static_cast<int>(job.endKind);
                node["pingAvg"] = job.pingAvg;
                node["pingMin"] = job.pingMin;
                node["pingMax"] = job.pingMax;
                node["pingSamples"] = job.pingSamples;

                if (job.stats.valid) {
                    json stats;
                    stats["kills"] = job.stats.kills;
                    stats["deaths"] = job.stats.deaths;
                    stats["assists"] = job.stats.assists;
                    stats["headShots"] = job.stats.headShots;
                    stats["damage"] = job.stats.damage;
                    stats["mvps"] = job.stats.mvps;
                    stats["roundsPlayed"] = job.stats.roundsPlayed;
                    stats["team"] = job.stats.team;
                    stats["teamChanges"] = job.stats.teamChanges;
                    if (job.stats.hasScore) stats["score"] = job.stats.score;
                    node["stats"] = stats;
                }
            }
            break;
    }

    // error_handler_t::replace, а не бросок на невалидном UTF-8: ник приходит
    // от игрока, и он вполне может прислать битую последовательность. Под
    // -fno-exceptions бросок превращается в std::abort() — то есть один игрок
    // с кривым ником уронил бы сервер.
    return node.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool DeserializeJob(const std::string& line, WriteJob* job) {
    const json node = json::parse(line, nullptr, false);
    if (node.is_discarded() || !node.is_object()) return false;

    const int kind = ReadOr(node, "kind", -1);
    if (kind < 0 || kind > 3) return false;

    WriteJob parsed;
    parsed.kind = static_cast<JobKind>(kind);
    parsed.attempts = ReadOr(node, "attempts", 0);
    parsed.serverId = ReadOr(node, "serverId", 1);

    switch (parsed.kind) {
        case JobKind::ServerUpsert:
            parsed.address = ReadOr<std::string>(node, "address", "");
            parsed.hostname = ReadOr<std::string>(node, "hostname", "");
            parsed.seenAt = ReadOr<int64_t>(node, "seenAt", 0);
            break;

        case JobKind::OnlineSnapshot:
            parsed.takenAt = ReadOr<int64_t>(node, "takenAt", 0);
            parsed.players = ReadOr(node, "players", 0);
            parsed.bots = ReadOr(node, "bots", 0);
            parsed.maxPlayers = ReadOr(node, "maxPlayers", 0);
            parsed.map = ReadOr<std::string>(node, "map", "");
            break;

        case JobKind::SessionOpen:
        case JobKind::SessionClose:
            parsed.sessionKey = ReadOr<std::string>(node, "sessionKey", "");
            if (parsed.sessionKey.empty()) return false;

            parsed.steamId64 = ReadOr<uint64_t>(node, "steamId64", 0);
            parsed.accountId = ReadOr<uint32_t>(node, "accountId", 0);
            parsed.nickname = ReadOr<std::string>(node, "nickname", "");
            parsed.startedAt = ReadOr<int64_t>(node, "startedAt", 0);

            if (parsed.kind == JobKind::SessionOpen) {
                parsed.connectMap = ReadOr<std::string>(node, "connectMap", "");
                parsed.playersOnline = ReadOr(node, "playersOnline", 0);
                parsed.maxPlayers = ReadOr(node, "maxPlayers", 0);
                parsed.pluginVersion = ReadOr<std::string>(node, "pluginVersion", "");
                ReadOptional(node, "clientLang", &parsed.hasClientLang, &parsed.clientLang);
                ReadOptional(node, "playerIp", &parsed.hasPlayerIp, &parsed.playerIp);
                ReadOptional(node, "ipHash", &parsed.hasIpHash, &parsed.ipHash);
                ReadOptional(node, "ipSubnet", &parsed.hasIpSubnet, &parsed.ipSubnet);
                ReadOptional(node, "countryIso", &parsed.hasCountryIso, &parsed.countryIso);
                ReadOptional(node, "countryName", &parsed.hasCountryName, &parsed.countryName);
                ReadOptional(node, "city", &parsed.hasCity, &parsed.city);
            } else {
                parsed.endedAt = ReadOr<int64_t>(node, "endedAt", 0);
                parsed.durationSeconds = ReadOr(node, "durationSeconds", 0);
                parsed.spectatorSeconds = ReadOr(node, "spectatorSeconds", 0);
                parsed.countSpectatorTime = ReadOr(node, "countSpectatorTime", true);
                parsed.disconnectMap = ReadOr<std::string>(node, "disconnectMap", "");
                parsed.disconnectReason = ReadOr(node, "disconnectReason", 0);
                parsed.disconnectReasonName =
                    ReadOr<std::string>(node, "disconnectReasonName", "");
                parsed.endKind = static_cast<SessionEndKind>(
                    ReadOr(node, "endKind", static_cast<int>(SessionEndKind::Disconnect)));
                parsed.pingAvg = ReadOr(node, "pingAvg", 0);
                parsed.pingMin = ReadOr(node, "pingMin", 0);
                parsed.pingMax = ReadOr(node, "pingMax", 0);
                parsed.pingSamples = ReadOr(node, "pingSamples", 0);

                const auto stats = node.find("stats");
                if (stats != node.end() && stats->is_object()) {
                    parsed.stats.valid = true;
                    parsed.stats.kills = ReadOr(*stats, "kills", 0);
                    parsed.stats.deaths = ReadOr(*stats, "deaths", 0);
                    parsed.stats.assists = ReadOr(*stats, "assists", 0);
                    parsed.stats.headShots = ReadOr(*stats, "headShots", 0);
                    parsed.stats.damage = ReadOr(*stats, "damage", 0);
                    parsed.stats.mvps = ReadOr(*stats, "mvps", 0);
                    parsed.stats.roundsPlayed = ReadOr(*stats, "roundsPlayed", 0);
                    parsed.stats.team = ReadOr(*stats, "team", 0);
                    parsed.stats.teamChanges = ReadOr(*stats, "teamChanges", 0);
                    if (stats->find("score") != stats->end()) {
                        parsed.stats.hasScore = true;
                        parsed.stats.score = ReadOr(*stats, "score", 0);
                    }
                }
            }
            break;
    }

    *job = parsed;
    return true;
}

bool Spool::Exists() const {
    struct stat info;
    return stat(_path.c_str(), &info) == 0;
}

int32_t Spool::CountLines() const {
    std::ifstream stream(_path.c_str());
    if (!stream.is_open()) return 0;

    int32_t count = 0;
    std::string line;
    while (std::getline(stream, line)) ++count;
    return count;
}

bool Spool::Append(const WriteJob& job) {
    if (CountLines() >= _maxEntries) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Спул достиг предела (" + std::to_string(_maxEntries) +
                           " записей) — задание отброшено. Проверьте доступность базы");
        }
        return false;
    }

    // Каталог создаётся здесь, а не при старте: пустые каталоги не переживают
    // упаковку в zip, поэтому на свежей установке addons/ConnectHistory/data
    // просто нет. Без этого первая же недоступность базы теряла задание —
    // ровно в тот момент, ради которого спул и существует.
    const std::string directory = fs::ParentDirectory(_path);
    if (!directory.empty() && !fs::EnsureDirectory(directory)) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Не удалось создать каталог спула: " + directory);
        }
        return false;
    }

    std::ofstream stream(_path.c_str(), std::ios::app | std::ios::binary);
    if (!stream.is_open()) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Не удалось записать задание в спул (" + _path +
                           "): " + job.Describe());
        }
        return false;
    }

    stream << SerializeJob(job) << "\n";
    stream.close();

    ProtectFile(_path);
    return true;
}

std::vector<WriteJob> Spool::TakeAll() {
    std::vector<WriteJob> jobs;

    std::ifstream stream(_path.c_str());
    if (!stream.is_open()) return jobs;

    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        WriteJob job;
        if (!DeserializeJob(line, &job)) {
            if (_logger != nullptr) _logger->Error("[DB] Битая строка в спуле пропущена");
            continue;
        }

        // Счётчик попыток обнуляем: это новая попытка, а не продолжение старой
        job.attempts = 0;
        jobs.push_back(job);
    }
    stream.close();

    std::remove(_path.c_str());
    return jobs;
}

}  // namespace ch
