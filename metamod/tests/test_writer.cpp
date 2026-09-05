// Писатель — самая опасная часть плагина: ретраи, спул и идемпотентное закрытие
// сессии. Заглушка базы позволяет проверить всё это без MySQL и, главное,
// воспроизвести падение базы, которое на живом сервере не устроишь по заказу.
#include "core/database.h"
#include "core/logger.h"
#include "core/writer.h"

#include "doctest.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class TempDir {
public:
    TempDir() {
        char pattern[] = "/tmp/ch_writer_testXXXXXX";
        const char* made = mkdtemp(pattern);
        _path = made != nullptr ? made : "/tmp/ch_writer_fallback";
    }
    ~TempDir() {
        const std::string command = "rm -rf '" + _path + "'";
        if (std::system(command.c_str()) != 0) { /* мусор в /tmp безвреден */ }
    }
    const std::string& Path() const { return _path; }

private:
    std::string _path;
};

class FakeDatabase : public ch::IDatabase {
public:
    std::string Target() const override { return "fake"; }
    std::string Prefix() const override { return "ch_"; }

    bool Ping(std::string* message) override {
        *message = "fake";
        return !failing;
    }

    bool Execute(const ch::Statement& statement, int* affectedRows,
                 std::string* error) override {
        executed.push_back(statement.sql);
        if (failing) {
            *error = "connection refused";
            return false;
        }
        if (affectedRows != nullptr) *affectedRows = updateAffects;
        return true;
    }

    bool Query(const ch::Statement& statement, std::vector<ch::ResultRow>* rows,
               std::string* error) override {
        executed.push_back(statement.sql);
        if (failing) {
            *error = "connection refused";
            return false;
        }
        if (sessionRowExists) {
            ch::ResultRow row;
            row.values.push_back("1");
            row.isNull.push_back(false);
            rows->push_back(row);
        }
        return true;
    }

    bool Begin(std::string* error) override {
        if (failing) {
            *error = "connection refused";
            return false;
        }
        ++begun;
        return true;
    }

    bool Commit(std::string* error) override {
        if (failing) {
            *error = "connection refused";
            return false;
        }
        ++committed;
        return true;
    }

    void Rollback() override { ++rolledBack; }

    unsigned int LastErrorCode() const override { return 0; }

    bool Contains(const std::string& fragment) const {
        for (const std::string& sql : executed) {
            if (sql.find(fragment) != std::string::npos) return true;
        }
        return false;
    }

    size_t CountContaining(const std::string& fragment) const {
        size_t count = 0;
        for (const std::string& sql : executed) {
            if (sql.find(fragment) != std::string::npos) ++count;
        }
        return count;
    }

    std::vector<std::string> executed;
    bool failing = false;
    int updateAffects = 1;
    bool sessionRowExists = false;
    int begun = 0;
    int committed = 0;
    int rolledBack = 0;
};

template <typename Fn>
bool WaitFor(Fn&& predicate, int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

ch::StorageSettings FastStorage() {
    ch::StorageSettings storage;
    storage.retryAttempts = 1;  // без пауз: ретраи проверяются отдельным тестом
    storage.retryDelaySeconds = 1;
    return storage;
}

ch::WriteJob CloseJob() {
    ch::WriteJob job;
    job.kind = ch::JobKind::SessionClose;
    job.sessionKey = "0123456789abcdef0123456789abcdef";
    job.steamId64 = 76561198000000001ull;
    job.accountId = 39734273u;
    job.nickname = "Игрок";
    job.startedAt = 1767268800;
    job.endedAt = 1767272400;
    job.durationSeconds = 3600;
    return job;
}

}  // namespace

TEST_CASE("Открытие сессии пишет строку и заводит игрока") {
    TempDir dir;
    ch::NullLogger logger;
    FakeDatabase database;

    ch::WriteJob job;
    job.kind = ch::JobKind::SessionOpen;
    job.sessionKey = "k";
    job.steamId64 = 76561198000000001ull;
    job.startedAt = 1767268800;

    {
        ch::SessionWriter writer(&database, FastStorage(), dir.Path(), &logger);
        writer.Enqueue(job);
        REQUIRE(WaitFor([&] { return writer.Written() == 1; }));
    }

    // Строка сессии создаётся на ВХОДЕ: упавший сервер иначе не оставляет
    // о сессии никаких следов вообще
    CHECK(database.Contains("INSERT IGNORE INTO `ch_sessions`"));
    // Игрок существует с первого захода, даже если сессия никогда не закроется
    CHECK(database.Contains("INSERT INTO `ch_players`"));
}

TEST_CASE("Обычное закрытие: UPDATE и агрегаты, без вставки") {
    TempDir dir;
    ch::NullLogger logger;
    FakeDatabase database;
    database.updateAffects = 1;

    {
        ch::SessionWriter writer(&database, FastStorage(), dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return writer.Written() == 1; }));
    }

    CHECK(database.Contains("UPDATE `ch_sessions` SET"));
    CHECK(database.Contains("INSERT INTO `ch_players`"));
    CHECK(database.Contains("INSERT INTO `ch_nicknames`"));
    CHECK_FALSE(database.Contains("INSERT IGNORE INTO `ch_sessions`"));
    CHECK(database.committed == 1);
    CHECK(database.rolledBack == 0);
}

TEST_CASE("Повторное закрытие из спула не удваивает наигранное время") {
    // Главный инвариант всей записи: задание могло прийти второй раз,
    // и счётчики трогать нельзя
    TempDir dir;
    ch::NullLogger logger;
    FakeDatabase database;
    database.updateAffects = 0;      // строка уже закрыта
    database.sessionRowExists = true;

    {
        ch::SessionWriter writer(&database, FastStorage(), dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return writer.Written() == 1; }));
    }

    CHECK(database.Contains("UPDATE `ch_sessions` SET"));
    CHECK_FALSE(database.Contains("INSERT INTO `ch_players`"));
    CHECK_FALSE(database.Contains("INSERT INTO `ch_nicknames`"));
    CHECK_FALSE(database.Contains("INSERT IGNORE INTO `ch_sessions`"));
    CHECK(database.committed == 1);
}

TEST_CASE("Потерянное открытие: вставляем полную строку") {
    TempDir dir;
    ch::NullLogger logger;
    FakeDatabase database;
    database.updateAffects = 0;       // строки нет
    database.sessionRowExists = false;

    {
        ch::SessionWriter writer(&database, FastStorage(), dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return writer.Written() == 1; }));
    }

    // Данные о сессии не должны пропасть совсем
    CHECK(database.Contains("INSERT IGNORE INTO `ch_sessions`"));
    CHECK(database.Contains("INSERT INTO `ch_players`"));
    CHECK(database.committed == 1);
}

TEST_CASE("Недоступная база: задание уходит в спул, а не теряется") {
    TempDir dir;
    ch::NullLogger logger;
    FakeDatabase database;
    database.failing = true;

    {
        ch::SessionWriter writer(&database, FastStorage(), dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return writer.Spooled() == 1; }));

        CHECK(writer.Written() == 0);
        CHECK(writer.SpoolExists());
        CHECK_FALSE(writer.LastError().empty());
    }
}

TEST_CASE("Восстановившаяся база забирает отложенное") {
    TempDir dir;
    ch::NullLogger logger;

    // Сначала копим спул на упавшей базе
    {
        FakeDatabase broken;
        broken.failing = true;

        ch::SessionWriter writer(&broken, FastStorage(), dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return writer.Spooled() == 1; }));
    }

    // Новый запуск с живой базой досылает спул первым делом
    FakeDatabase healthy;
    {
        ch::SessionWriter writer(&healthy, FastStorage(), dir.Path(), &logger);
        REQUIRE(WaitFor([&] { return writer.Written() >= 1; }));
    }

    CHECK(healthy.Contains("UPDATE `ch_sessions` SET"));
}

TEST_CASE("Сбой посреди транзакции откатывается") {
    TempDir dir;
    ch::NullLogger logger;

    class FailOnAggregates : public FakeDatabase {
    public:
        bool Execute(const ch::Statement& statement, int* affectedRows,
                     std::string* error) override {
            if (statement.sql.find("`ch_players`") != std::string::npos) {
                *error = "deadlock";
                return false;
            }
            return FakeDatabase::Execute(statement, affectedRows, error);
        }
    };

    FailOnAggregates database;
    {
        ch::SessionWriter writer(&database, FastStorage(), dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return writer.Spooled() == 1; }));
    }

    // Полусохранённая сессия хуже отложенной: агрегат разъедется с историей
    CHECK(database.rolledBack == 1);
    CHECK(database.committed == 0);
}

TEST_CASE("Ретраи повторяют попытку, а не сдаются сразу") {
    TempDir dir;
    ch::NullLogger logger;

    class FailTwice : public FakeDatabase {
    public:
        bool Begin(std::string* error) override {
            ++calls;
            if (calls <= 2) {
                *error = "server has gone away";
                return false;
            }
            return FakeDatabase::Begin(error);
        }
        int calls = 0;
    };

    FailTwice database;
    ch::StorageSettings storage;
    storage.retryAttempts = 3;
    storage.retryDelaySeconds = 1;

    {
        ch::SessionWriter writer(&database, storage, dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return writer.Written() == 1; }, 15000));
    }

    CHECK(database.calls == 3);
    CHECK(database.committed == 1);
}

TEST_CASE("Выключенный спул теряет задание явно, а не молча") {
    TempDir dir;
    ch::NullLogger logger;
    FakeDatabase database;
    database.failing = true;

    ch::StorageSettings storage = FastStorage();
    storage.spoolEnabled = false;

    {
        ch::SessionWriter writer(&database, storage, dir.Path(), &logger);
        writer.Enqueue(CloseJob());
        REQUIRE(WaitFor([&] { return !writer.LastError().empty(); }));
        CHECK(writer.Spooled() == 0);
        CHECK_FALSE(writer.SpoolExists());
    }
}

TEST_CASE("Остановка не теряет очередь") {
    TempDir dir;
    ch::NullLogger logger;
    FakeDatabase database;
    database.failing = true;

    ch::SessionWriter writer(&database, FastStorage(), dir.Path(), &logger);
    for (int i = 0; i < 5; ++i) writer.Enqueue(CloseJob());

    writer.Stop();

    // Всё, что не успело записаться, обязано оказаться в спуле
    CHECK(writer.Spooled() == 5);
    CHECK(writer.SpoolExists());
}
