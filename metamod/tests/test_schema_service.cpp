// Подготовка схемы: порядок DDL, чтение версии, миграции и их идемпотентность.
#include "core/database.h"
#include "core/logger.h"
#include "core/schema_service.h"
#include "core/schema_sql.h"

#include "doctest.h"

#include <string>
#include <vector>

namespace {

class FakeSchemaDatabase : public ch::IDatabase {
public:
    std::string Target() const override { return "fake"; }
    std::string Prefix() const override { return "ch_"; }
    bool Ping(std::string*) override { return true; }

    bool Execute(const ch::Statement& statement, int* affectedRows,
                 std::string* error) override {
        executed.push_back(statement.sql);

        if (failAlterAsDuplicate && statement.sql.find("ADD COLUMN") != std::string::npos) {
            lastError = 1060;  // duplicate column
            *error = "Duplicate column name 'spectator_seconds'";
            return false;
        }
        if (failAlterHard && statement.sql.find("ADD COLUMN") != std::string::npos) {
            lastError = 1142;  // отказано в правах
            *error = "ALTER command denied";
            return false;
        }
        if (failEverything) {
            lastError = 2002;
            *error = "Can't connect to MySQL server";
            return false;
        }

        lastError = 0;
        if (affectedRows != nullptr) *affectedRows = staleRows;
        return true;
    }

    bool Query(const ch::Statement& statement, std::vector<ch::ResultRow>* rows,
               std::string*) override {
        executed.push_back(statement.sql);
        if (storedVersion < 0) return true;  // строки нет — свежая база

        ch::ResultRow row;
        row.values.push_back(std::to_string(storedVersion));
        row.isNull.push_back(false);
        rows->push_back(row);
        return true;
    }

    bool Begin(std::string*) override { return true; }
    bool Commit(std::string*) override { return true; }
    void Rollback() override {}
    unsigned int LastErrorCode() const override { return lastError; }

    size_t CountContaining(const std::string& fragment) const {
        size_t count = 0;
        for (const std::string& sql : executed) {
            if (sql.find(fragment) != std::string::npos) ++count;
        }
        return count;
    }

    bool Contains(const std::string& fragment) const {
        return CountContaining(fragment) > 0;
    }

    std::vector<std::string> executed;
    int storedVersion = -1;
    int staleRows = 0;
    bool failEverything = false;
    bool failAlterAsDuplicate = false;
    bool failAlterHard = false;
    unsigned int lastError = 0;
};

}  // namespace

TEST_CASE("Свежая база: все таблицы и запись версии") {
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    ch::SchemaService schema(&database, &logger);

    CHECK(schema.EnsureSchema());

    CHECK(database.CountContaining("CREATE TABLE IF NOT EXISTS") == 6);
    CHECK(database.Contains("`ch_schema_version`"));
    CHECK(database.Contains("INSERT INTO `ch_schema_version`"));
}

TEST_CASE("Повторный прогон на актуальной базе не мигрирует ничего") {
    // Плагин перезагружают на живом сервере
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    database.storedVersion = ch::kSchemaVersion;

    ch::SchemaService schema(&database, &logger);
    CHECK(schema.EnsureSchema());

    CHECK_FALSE(database.Contains("ADD COLUMN"));
    CHECK_FALSE(database.Contains("UPDATE `ch_servers`"));
}

TEST_CASE("Старая база проходит все недостающие шаги") {
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    database.storedVersion = 1;

    ch::SchemaService schema(&database, &logger);
    CHECK(schema.EnsureSchema());

    // v2 чистит адрес привязки, v3 добавляет spectator_seconds
    CHECK(database.Contains("UPDATE `ch_servers`"));
    CHECK(database.Contains("ADD COLUMN `spectator_seconds`"));
}

TEST_CASE("«Колонка уже существует» — это применённый шаг") {
    // У ALTER в MySQL нет IF NOT EXISTS, а на свежей базе колонка уже приезжает
    // из BuildSchema
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    database.storedVersion = 2;
    database.failAlterAsDuplicate = true;

    ch::SchemaService schema(&database, &logger);
    CHECK(schema.EnsureSchema());
    CHECK(database.Contains("INSERT INTO `ch_schema_version`"));
}

TEST_CASE("Настоящая ошибка миграции останавливает подготовку схемы") {
    // Молча проглоченная миграция страшнее упавшей: вторая видна сразу,
    // первая всплывёт неверными данными
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    database.storedVersion = 2;
    database.failAlterHard = true;

    ch::SchemaService schema(&database, &logger);
    CHECK_FALSE(schema.EnsureSchema());
    CHECK_FALSE(database.Contains("INSERT INTO `ch_schema_version`"));
}

TEST_CASE("База новее плагина: не ломаем и не мигрируем назад") {
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    database.storedVersion = ch::kSchemaVersion + 5;

    ch::SchemaService schema(&database, &logger);
    CHECK(schema.EnsureSchema());
    CHECK_FALSE(database.Contains("INSERT INTO `ch_schema_version`"));
}

TEST_CASE("Недоступная база не роняет плагин") {
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    database.failEverything = true;

    ch::SchemaService schema(&database, &logger);
    CHECK_FALSE(schema.EnsureSchema());
}

TEST_CASE("Оборванные сессии помечаются, а не удаляются") {
    // Карта падений сервера обязана остаться в данных
    ch::NullLogger logger;
    FakeSchemaDatabase database;
    database.staleRows = 4;

    ch::SchemaService schema(&database, &logger);
    CHECK(schema.MarkStaleSessions(7) == 4);

    CHECK(database.Contains("UPDATE `ch_sessions` SET `end_kind` = ?"));
    CHECK(database.Contains("`ended_at` IS NULL AND `end_kind` = 0"));
    CHECK_FALSE(database.Contains("DELETE"));
}
