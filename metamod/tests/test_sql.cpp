// Тексты запросов и решения о том, какой из них выполнять.
//
// Проверяется здесь именно то, что нельзя проверить, глядя на живую базу постфактум:
// идемпотентность закрытия сессии, «пустой адрес не затирает записанный», число
// плейсхолдеров под число параметров.
#include "core/schema_sql.h"
#include "core/util/steamid.h"
#include "core/sql_builder.h"

#include "doctest.h"

#include <algorithm>

namespace {

size_t CountPlaceholders(const std::string& sql) {
    return static_cast<size_t>(std::count(sql.begin(), sql.end(), '?'));
}

ch::WriteJob CloseJob() {
    ch::WriteJob job;
    job.kind = ch::JobKind::SessionClose;
    job.sessionKey = "0123456789abcdef0123456789abcdef";
    job.steamId64 = 76561198000000001ull;
    job.accountId = ch::ToAccountId(job.steamId64);
    job.serverId = 1;
    job.nickname = "Игрок";
    job.startedAt = 1767268800;
    job.endedAt = 1767272400;
    job.durationSeconds = 3600;
    job.spectatorSeconds = 1800;
    job.countSpectatorTime = false;
    job.disconnectMap = "de_dust2";
    job.endKind = ch::SessionEndKind::Disconnect;
    job.pingSamples = 0;
    return job;
}

}  // namespace

TEST_CASE("Схема создаётся идемпотентно и знает про свою версию") {
    const std::vector<std::string> ddl = ch::BuildSchema("ch_");
    CHECK(ddl.size() == 6);

    for (const std::string& sql : ddl) {
        // Плагин перезагружают на живом сервере — повторный прогон DDL обязан пройти
        CHECK(sql.find("CREATE TABLE IF NOT EXISTS") == 0);
        // Внешних ключей нет сознательно: они связали бы историю с агрегатами
        CHECK(sql.find("FOREIGN KEY") == std::string::npos);
    }

    // Префикс подставляется во все таблицы
    const std::vector<std::string> prefixed = ch::BuildSchema("stats2_");
    for (const std::string& sql : prefixed) {
        CHECK(sql.find("`stats2_") != std::string::npos);
    }
}

TEST_CASE("Версия схемы не отстаёт от лестницы миграций") {
    const std::vector<ch::MigrationStep> steps = ch::Migrations("ch_");
    REQUIRE_FALSE(steps.empty());

    int highest = 0;
    int previous = 1;
    for (const ch::MigrationStep& step : steps) {
        // Каждый шаг достижим: лестница без пропусков
        CHECK(step.target == previous + 1);
        previous = step.target;
        highest = std::max(highest, step.target);
    }

    CHECK(highest == ch::kSchemaVersion);
}

TEST_CASE("«Объект уже существует» — это применённый шаг, а не сбой") {
    CHECK(ch::IsAlreadyAppliedError(1050));  // таблица
    CHECK(ch::IsAlreadyAppliedError(1060));  // колонка
    CHECK(ch::IsAlreadyAppliedError(1061));  // индекс

    // Всё прочее обязано всплывать: молча проглоченная миграция страшнее упавшей
    CHECK_FALSE(ch::IsAlreadyAppliedError(1045));  // access denied
    CHECK_FALSE(ch::IsAlreadyAppliedError(1146));  // таблицы нет
    CHECK_FALSE(ch::IsAlreadyAppliedError(2006));  // сервер ушёл
}

TEST_CASE("Пустой адрес не затирает уже записанный") {
    // Корень проблемы «в базе 0.0.0.0, и правка руками не держится»
    const std::string sql = ch::ServerUpsertSql("ch_");

    CHECK(sql.find("IF(VALUES(`address`) = '', `address`, VALUES(`address`))") !=
          std::string::npos);
    CHECK(sql.find("IF(VALUES(`hostname`) = '', `hostname`, VALUES(`hostname`))") !=
          std::string::npos);
    CHECK(sql.find("`ch_servers`") != std::string::npos);
}

TEST_CASE("Число параметров совпадает с числом плейсхолдеров") {
    // Расхождение здесь — это либо потерянная колонка, либо сдвиг значений
    // на одну позицию: и то и другое база примет молча
    ch::WriteJob server;
    server.kind = ch::JobKind::ServerUpsert;
    server.address = "203.0.113.10:27015";
    server.hostname = "Test";
    server.seenAt = 1767268800;

    const ch::Statement upsert = ch::BuildServerUpsert("ch_", server);
    CHECK(CountPlaceholders(upsert.sql) == upsert.params.size());

    ch::WriteJob open;
    open.kind = ch::JobKind::SessionOpen;
    open.sessionKey = "k";
    open.startedAt = 1767268800;
    for (const ch::Statement& statement : ch::BuildSessionOpen("ch_", open)) {
        CHECK(CountPlaceholders(statement.sql) == statement.params.size());
    }

    const ch::WriteJob close = CloseJob();
    const ch::Statement update = ch::BuildSessionCloseUpdate("ch_", close);
    CHECK(CountPlaceholders(update.sql) == update.params.size());

    const ch::Statement insert = ch::BuildSessionCloseInsert("ch_", close);
    CHECK(CountPlaceholders(insert.sql) == insert.params.size());

    for (const ch::Statement& statement : ch::BuildAggregates("ch_", close)) {
        CHECK(CountPlaceholders(statement.sql) == statement.params.size());
    }

    ch::WriteJob snapshot;
    snapshot.kind = ch::JobKind::OnlineSnapshot;
    snapshot.takenAt = 1767268800;
    const ch::Statement snap = ch::BuildOnlineSnapshot("ch_", snapshot);
    CHECK(CountPlaceholders(snap.sql) == snap.params.size());
}

TEST_CASE("Закрытие сессии идемпотентно") {
    // UPDATE сработал — обновляем агрегаты
    CHECK(ch::PlanClose(1, false) == ch::CloseAction::UpdateAggregates);

    // Строка есть, но UPDATE ничего не изменил: сессия уже закрыта, задание пришло
    // повторно из спула. Счётчики трогать нельзя — иначе наигранное время удвоится
    CHECK(ch::PlanClose(0, true) == ch::CloseAction::AlreadyClosed);

    // Строки открытия нет вовсе (её задание потерялось) — вставляем полную
    CHECK(ch::PlanClose(0, false) == ch::CloseAction::InsertThenAggregates);
}

TEST_CASE("UPDATE закрытия пишет spectator_seconds") {
    // Обычное закрытие сессии идёт именно этим UPDATE. В C#-версии колонки здесь
    // не было, и время вне игры оставалось NULL у каждой нормально закрытой сессии
    const ch::Statement update = ch::BuildSessionCloseUpdate("ch_", CloseJob());

    CHECK(update.sql.find("`spectator_seconds` = ?") != std::string::npos);
    CHECK(update.sql.find("`ended_at` IS NULL") != std::string::npos);
}

TEST_CASE("Настройка «не считать наблюдателя» влияет на агрегат, а не на сессию") {
    ch::WriteJob job = CloseJob();

    job.countSpectatorTime = false;
    const std::vector<ch::Statement> without = ch::BuildAggregates("ch_", job);
    REQUIRE_FALSE(without.empty());
    // total_seconds = 3600 - 1800
    CHECK(without[0].params[4].integer == 1800);

    job.countSpectatorTime = true;
    const std::vector<ch::Statement> with = ch::BuildAggregates("ch_", job);
    CHECK(with[0].params[4].integer == 3600);

    // А duration_seconds в самой сессии — всегда честное время подключения
    const ch::Statement update = ch::BuildSessionCloseUpdate("ch_", job);
    CHECK(update.params[1].integer == 3600);
    CHECK(update.params[2].integer == 1800);
}

TEST_CASE("Пустой ник не плодит строку в истории ников") {
    ch::WriteJob job = CloseJob();
    CHECK(ch::BuildAggregates("ch_", job).size() == 2);

    job.nickname.clear();
    CHECK(ch::BuildAggregates("ch_", job).size() == 1);
}

TEST_CASE("Ноль замеров пинга пишется как NULL, а не как ноль") {
    // Ноль в колонке неотличим от «связь идеальная»
    ch::WriteJob job = CloseJob();
    job.pingSamples = 0;

    const ch::Statement empty = ch::BuildSessionCloseUpdate("ch_", job);
    CHECK(empty.params[18].kind == ch::SqlValue::Kind::Null);  // ping_avg

    job.pingSamples = 5;
    job.pingAvg = 42;
    const ch::Statement measured = ch::BuildSessionCloseUpdate("ch_", job);
    CHECK(measured.params[18].kind == ch::SqlValue::Kind::Int64);
    CHECK(measured.params[18].integer == 42);
}

TEST_CASE("score остаётся NULL: в этой цели его неоткуда взять") {
    // В CS2 нет игрового события со счётом, а чтение поля контроллера требует
    // указателя на CGameEntitySystem по захардкоженному смещению — цена не та
    ch::WriteJob job = CloseJob();
    job.stats.valid = true;
    job.stats.kills = 10;

    const ch::Statement update = ch::BuildSessionCloseUpdate("ch_", job);
    CHECK(update.params[8].integer == 10);                    // kills
    CHECK(update.params[14].kind == ch::SqlValue::Kind::Null); // score
}

TEST_CASE("Идентификаторы не склеиваются со значениями") {
    // Префикс — единственное, что попадает в текст запроса; всё остальное
    // уходит параметрами. Ник игрока в тексте SQL означал бы инъекцию
    ch::WriteJob job = CloseJob();
    job.nickname = "'; DROP TABLE ch_sessions; --";

    const ch::Statement update = ch::BuildSessionCloseUpdate("ch_", job);
    CHECK(update.sql.find("DROP TABLE") == std::string::npos);
    CHECK(update.params[4].text == job.nickname);
}
