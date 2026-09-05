// Чтение из базы для игроцких команд.
//
// Отдельно от писателя сознательно: писатель обязан оставаться одной
// последовательной очередью, а команды игроков — это редкие независимые запросы,
// которым незачем вставать в неё за сессиями.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ch {

class IDatabase;
class ILogger;

struct PlayerTotals {
    bool found = false;
    int64_t totalSeconds = 0;
    int32_t sessions = 0;
    int64_t firstSeen = 0;  // секунды эпохи, UTC
};

struct RecentSession {
    int64_t startedAt = 0;  // секунды эпохи, UTC
    int32_t durationSeconds = 0;
    std::string map;
};

// Разбор "YYYY-MM-DD HH:MM:SS" из базы в секунды эпохи UTC.
// В базе время всегда в UTC — это соглашение всего плагина.
int64_t ParseSqlDateTime(const std::string& value);

class QueryService {
public:
    QueryService(IDatabase* database, ILogger* logger)
        : _database(database), _logger(logger) {}

    PlayerTotals GetTotals(uint64_t steamId);

    // Пустой список — либо данных нет, либо запрос не удался; для чата разницы нет.
    std::vector<RecentSession> GetRecent(uint64_t steamId, int limit);

private:
    IDatabase* _database;
    ILogger* _logger;
};

}  // namespace ch
