// Клиент MySQL/MariaDB.
//
// Параметры подключения передаются mysql_real_connect ОТДЕЛЬНЫМИ аргументами,
// а не строкой. Это не стилистика: в C#-целях строку подключения обязан собирать
// билдер, потому что пароль с ';' в интерполяции подменяет параметры подключения.
// Здесь такой строки нет вовсе — подмене нечего подменять.
#pragma once

#include "core/config.h"
#include "core/database.h"

#include <string>

struct st_mysql;

namespace ch {

class ILogger;

class MariaDatabase final : public IDatabase {
public:
    MariaDatabase(const DatabaseSettings& settings, ILogger* logger);
    ~MariaDatabase() override;

    MariaDatabase(const MariaDatabase&) = delete;
    MariaDatabase& operator=(const MariaDatabase&) = delete;

    std::string Target() const override { return _target; }
    std::string Prefix() const override { return _prefix; }

    bool Ping(std::string* message) override;

    bool Execute(const Statement& statement, int* affectedRows, std::string* error) override;
    bool Query(const Statement& statement, std::vector<ResultRow>* rows,
               std::string* error) override;

    bool Begin(std::string* error) override;
    bool Commit(std::string* error) override;
    void Rollback() override;

    unsigned int LastErrorCode() const override { return _lastErrorCode; }

    void LogTarget();

private:
    // Соединение поднимается лениво: Load() плагина не должен ждать сеть.
    bool Ensure(std::string* error);
    void Close();

    // true, если ошибка означает «соединение умерло» и стоит переподключиться.
    // Игровой сервер живёт неделями, а база за это время перезагружается.
    static bool IsConnectionLost(unsigned int code);

    bool Run(const Statement& statement, int* affectedRows, std::vector<ResultRow>* rows,
             std::string* error, bool allowRetry);

    DatabaseSettings _settings;
    ILogger* _logger;
    std::string _prefix;
    std::string _target;

    st_mysql* _handle = nullptr;
    unsigned int _lastErrorCode = 0;
    bool _inTransaction = false;
};

}  // namespace ch
