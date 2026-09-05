// Чтение для игроцких команд.
#include "core/database.h"
#include "core/logger.h"
#include "core/query_service.h"

#include "doctest.h"

#include <string>
#include <vector>

namespace {

class FakeReadDatabase : public ch::IDatabase {
public:
    std::string Target() const override { return "fake"; }
    std::string Prefix() const override { return "ch_"; }
    bool Ping(std::string*) override { return true; }
    bool Execute(const ch::Statement&, int*, std::string*) override { return true; }

    bool Query(const ch::Statement& statement, std::vector<ch::ResultRow>* out,
               std::string* error) override {
        lastSql = statement.sql;
        lastParams = statement.params;
        if (failing) {
            *error = "connection refused";
            return false;
        }
        *out = rows;
        return true;
    }

    bool Begin(std::string*) override { return true; }
    bool Commit(std::string*) override { return true; }
    void Rollback() override {}
    unsigned int LastErrorCode() const override { return 0; }

    std::string lastSql;
    std::vector<ch::SqlValue> lastParams;
    std::vector<ch::ResultRow> rows;
    bool failing = false;
};

ch::ResultRow Row(const std::vector<std::string>& values,
                  const std::vector<bool>& nulls = {}) {
    ch::ResultRow row;
    row.values = values;
    row.isNull = nulls.empty() ? std::vector<bool>(values.size(), false) : nulls;
    return row;
}

}  // namespace

TEST_CASE("Время из базы разбирается как UTC") {
    // В базе время всегда в UTC — это соглашение всего плагина
    CHECK(ch::ParseSqlDateTime("2026-09-05 12:34:56") == 1788611696);
    CHECK(ch::ParseSqlDateTime("мусор") == 0);
}

TEST_CASE("Наигранное время читается по SteamID") {
    ch::NullLogger logger;
    FakeReadDatabase database;
    database.rows.push_back(Row({"7200", "12", "2026-09-05 12:34:56"}));

    ch::QueryService service(&database, &logger);
    const ch::PlayerTotals totals = service.GetTotals(76561198000000001ull);

    CHECK(totals.found);
    CHECK(totals.totalSeconds == 7200);
    CHECK(totals.sessions == 12);
    CHECK(totals.firstSeen == 1788611696);

    // SteamID уходит параметром, а не в текст запроса
    CHECK(database.lastSql.find("76561198000000001") == std::string::npos);
    REQUIRE(database.lastParams.size() == 1);
    CHECK(database.lastParams[0].unsignedInteger == 76561198000000001ull);
}

TEST_CASE("Игрока нет в базе — это не ошибка") {
    ch::NullLogger logger;
    FakeReadDatabase database;

    ch::QueryService service(&database, &logger);
    CHECK_FALSE(service.GetTotals(76561198000000001ull).found);
}

TEST_CASE("Недоступная база не роняет команду") {
    ch::NullLogger logger;
    FakeReadDatabase database;
    database.failing = true;

    ch::QueryService service(&database, &logger);
    CHECK_FALSE(service.GetTotals(1).found);
    CHECK(service.GetRecent(1, 5).empty());
}

TEST_CASE("Последние заходы: NULL в колонках не роняет разбор") {
    ch::NullLogger logger;
    FakeReadDatabase database;
    database.rows.push_back(Row({"2026-09-05 12:34:56", "3600", "de_dust2"}));
    database.rows.push_back(Row({"2026-09-04 10:00:00", "0", ""}, {false, true, true}));

    ch::QueryService service(&database, &logger);
    const std::vector<ch::RecentSession> sessions = service.GetRecent(1, 5);

    REQUIRE(sessions.size() == 2);
    CHECK(sessions[0].durationSeconds == 3600);
    CHECK(sessions[0].map == "de_dust2");
    CHECK(sessions[1].durationSeconds == 0);
    CHECK(sessions[1].map.empty());

    // Показываем только завершённые сессии: у открытой нет длительности
    CHECK(database.lastSql.find("`ended_at` IS NOT NULL") != std::string::npos);
}

TEST_CASE("LIMIT параметризован и ограничен сверху") {
    ch::NullLogger logger;
    FakeReadDatabase database;
    ch::QueryService service(&database, &logger);

    service.GetRecent(1, 1000);
    REQUIRE(database.lastParams.size() == 2);
    CHECK(database.lastParams[1].integer == 20);

    service.GetRecent(1, 0);
    CHECK(database.lastParams[1].integer == 1);

    CHECK(database.lastSql.find("LIMIT ?") != std::string::npos);
}
