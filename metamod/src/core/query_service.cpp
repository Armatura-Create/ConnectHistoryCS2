#include "core/query_service.h"

#include "core/database.h"
#include "core/logger.h"
#include "core/util/sql_sanitizer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace ch {

int64_t ParseSqlDateTime(const std::string& value) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour,
                    &minute, &second) < 3) {
        return 0;
    }

    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
    parts.tm_isdst = 0;

#ifdef _WIN32
    return static_cast<int64_t>(_mkgmtime(&parts));
#else
    return static_cast<int64_t>(timegm(&parts));
#endif
}

PlayerTotals QueryService::GetTotals(uint64_t steamId) {
    PlayerTotals totals;

    Statement statement;
    statement.sql = "SELECT `total_seconds`, `sessions_count`, `first_seen` FROM `" +
                    _database->Prefix() + "players` WHERE `steamid64` = ?";
    statement.params = {SqlValue::UInt(steamId)};

    std::vector<ResultRow> rows;
    std::string error;
    if (!_database->Query(statement, &rows, &error)) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Запрос наигранного времени для " + std::to_string(steamId) +
                           " не удался: " + MaskSecrets(error));
        }
        return totals;
    }

    if (rows.empty() || rows[0].values.size() < 3) return totals;

    totals.found = true;
    totals.totalSeconds = std::strtoll(rows[0].values[0].c_str(), nullptr, 10);
    totals.sessions = std::atoi(rows[0].values[1].c_str());
    totals.firstSeen = ParseSqlDateTime(rows[0].values[2]);
    return totals;
}

std::vector<RecentSession> QueryService::GetRecent(uint64_t steamId, int limit) {
    std::vector<RecentSession> result;

    Statement statement;
    statement.sql = "SELECT `started_at`, `duration_seconds`, `connect_map` FROM `" +
                    _database->Prefix() +
                    "sessions` WHERE `steamid64` = ? AND `ended_at` IS NOT NULL "
                    "ORDER BY `started_at` DESC LIMIT ?";
    // LIMIT параметризован: значение приходит из конфига, а не из чата, но
    // склеивать числа в SQL — привычка, которая однажды прострелит ногу.
    statement.params = {
        SqlValue::UInt(steamId),
        SqlValue::Int(std::min(20, std::max(1, limit))),
    };

    std::vector<ResultRow> rows;
    std::string error;
    if (!_database->Query(statement, &rows, &error)) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Запрос последних сессий для " + std::to_string(steamId) +
                           " не удался: " + MaskSecrets(error));
        }
        return result;
    }

    for (const ResultRow& row : rows) {
        if (row.values.size() < 3) continue;

        RecentSession session;
        session.startedAt = ParseSqlDateTime(row.values[0]);
        const bool durationNull = row.isNull.size() > 1 && row.isNull[1];
        session.durationSeconds = durationNull ? 0 : std::atoi(row.values[1].c_str());
        const bool mapNull = row.isNull.size() > 2 && row.isNull[2];
        session.map = mapNull ? std::string() : row.values[2];

        result.push_back(session);
    }

    return result;
}

}  // namespace ch
