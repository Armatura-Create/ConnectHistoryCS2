#include "db/mariadb.h"

#include "core/logger.h"
#include "core/util/sql_sanitizer.h"

#include <mysql.h>

#include <cstring>
#include <vector>

namespace ch {
namespace {

// Размер буфера под одно значение результата. Всё, что плагин читает, короче:
// самая длинная колонка — connect_map VARCHAR(64). Обрезка всё равно
// обрабатывается: молча укороченное значение хуже лишнего чтения.
constexpr unsigned long kValueBuffer = 512;

// «Сервер ушёл» и «соединение потеряно во время запроса»
constexpr unsigned int kServerGoneAway = 2006;
constexpr unsigned int kLostConnection = 2013;

}  // namespace

MariaDatabase::MariaDatabase(const DatabaseSettings& settings, ILogger* logger)
    : _settings(settings), _logger(logger) {
    _prefix = SanitizePrefix(settings.tablePrefix, logger);
    _target = settings.host + ":" + std::to_string(settings.port) + "/" + settings.database +
              " (user=" + settings.user + ", ssl=" + settings.sslMode + ")";
}

MariaDatabase::~MariaDatabase() { Close(); }

void MariaDatabase::LogTarget() {
    if (_logger != nullptr) {
        _logger->Info("[DB] Цель: " + _target + ", префикс таблиц \"" + _prefix + "\"");
    }
}

void MariaDatabase::Close() {
    if (_handle != nullptr) {
        mysql_close(_handle);
        _handle = nullptr;
    }
    _inTransaction = false;
}

bool MariaDatabase::IsConnectionLost(unsigned int code) {
    return code == kServerGoneAway || code == kLostConnection;
}

bool MariaDatabase::Ensure(std::string* error) {
    if (_handle != nullptr) return true;

    _handle = mysql_init(nullptr);
    if (_handle == nullptr) {
        *error = "mysql_init вернул NULL";
        return false;
    }

    unsigned int connectTimeout =
        _settings.connectionTimeoutSeconds == 0 ? 10u : _settings.connectionTimeoutSeconds;
    unsigned int commandTimeout =
        _settings.commandTimeoutSeconds == 0 ? 30u : _settings.commandTimeoutSeconds;

    // Без таймаутов зависшая сеть превращается в вечно висящий фоновый поток
    mysql_optionsv(_handle, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    mysql_optionsv(_handle, MYSQL_OPT_READ_TIMEOUT, &commandTimeout);
    mysql_optionsv(_handle, MYSQL_OPT_WRITE_TIMEOUT, &commandTimeout);
    mysql_optionsv(_handle, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    // Неизвестное значение из конфига не должно молча снижать защиту: всё,
    // что не "None", трактуется как «шифрование желательно», а "Required" —
    // как «без него не подключаемся».
    if (_settings.sslMode == "Required" || _settings.sslMode == "required") {
        my_bool enforce = 1;
        mysql_optionsv(_handle, MYSQL_OPT_SSL_ENFORCE, &enforce);
    }

    const char* user = _settings.user.empty() ? nullptr : _settings.user.c_str();
    const char* password = _settings.password.empty() ? nullptr : _settings.password.c_str();

    if (mysql_real_connect(_handle, _settings.host.c_str(), user, password,
                           _settings.database.c_str(), _settings.port, nullptr,
                           0) == nullptr) {
        _lastErrorCode = mysql_errno(_handle);
        *error = mysql_error(_handle);
        Close();
        return false;
    }

    mysql_set_character_set(_handle, "utf8mb4");
    _lastErrorCode = 0;
    return true;
}

bool MariaDatabase::Run(const Statement& statement, int* affectedRows,
                        std::vector<ResultRow>* rows, std::string* error,
                        bool allowRetry) {
    if (!Ensure(error)) return false;

    MYSQL_STMT* stmt = mysql_stmt_init(_handle);
    if (stmt == nullptr) {
        _lastErrorCode = mysql_errno(_handle);
        *error = mysql_error(_handle);
        return false;
    }

    if (mysql_stmt_prepare(stmt, statement.sql.c_str(),
                           static_cast<unsigned long>(statement.sql.size())) != 0) {
        _lastErrorCode = mysql_stmt_errno(stmt);
        *error = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);

        // Соединение могло умереть, пока на сервере никого не было
        if (allowRetry && IsConnectionLost(_lastErrorCode) && !_inTransaction) {
            Close();
            return Run(statement, affectedRows, rows, error, false);
        }
        return false;
    }

    // Значения ВСЕГДА уходят параметрами. Единственное, что попадает в текст
    // запроса, — префикс таблиц, и он прошёл белый список.
    std::vector<MYSQL_BIND> binds(statement.params.size());
    std::vector<my_bool> nulls(statement.params.size(), 0);
    std::vector<long long> integers(statement.params.size(), 0);
    std::vector<unsigned long long> unsignedIntegers(statement.params.size(), 0);
    std::vector<unsigned long> lengths(statement.params.size(), 0);

    if (!binds.empty()) std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * binds.size());

    for (size_t i = 0; i < statement.params.size(); ++i) {
        const SqlValue& value = statement.params[i];

        switch (value.kind) {
            case SqlValue::Kind::Null:
                nulls[i] = 1;
                binds[i].buffer_type = MYSQL_TYPE_NULL;
                binds[i].is_null = &nulls[i];
                break;

            case SqlValue::Kind::Int64:
                integers[i] = static_cast<long long>(value.integer);
                binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                binds[i].buffer = &integers[i];
                break;

            case SqlValue::Kind::UInt64:
                unsignedIntegers[i] = value.unsignedInteger;
                binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                binds[i].buffer = &unsignedIntegers[i];
                binds[i].is_unsigned = 1;
                break;

            case SqlValue::Kind::Text:
                lengths[i] = static_cast<unsigned long>(value.text.size());
                binds[i].buffer_type = MYSQL_TYPE_STRING;
                binds[i].buffer = const_cast<char*>(value.text.c_str());
                binds[i].buffer_length = lengths[i];
                binds[i].length = &lengths[i];
                break;
        }
    }

    if (!binds.empty() && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        _lastErrorCode = mysql_stmt_errno(stmt);
        *error = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        _lastErrorCode = mysql_stmt_errno(stmt);
        *error = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);

        if (allowRetry && IsConnectionLost(_lastErrorCode) && !_inTransaction) {
            Close();
            return Run(statement, affectedRows, rows, error, false);
        }
        return false;
    }

    _lastErrorCode = 0;

    if (affectedRows != nullptr) {
        affectedRows[0] = static_cast<int>(mysql_stmt_affected_rows(stmt));
    }

    if (rows == nullptr) {
        mysql_stmt_close(stmt);
        return true;
    }

    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (meta == nullptr) {
        mysql_stmt_close(stmt);
        return true;  // запрос без результата — это не ошибка
    }

    const unsigned int columns = mysql_num_fields(meta);
    mysql_free_result(meta);

    std::vector<MYSQL_BIND> resultBinds(columns);
    std::vector<std::vector<char> > buffers(columns, std::vector<char>(kValueBuffer));
    std::vector<unsigned long> resultLengths(columns, 0);
    std::vector<my_bool> resultNulls(columns, 0);
    std::vector<my_bool> resultErrors(columns, 0);

    std::memset(resultBinds.data(), 0, sizeof(MYSQL_BIND) * columns);
    for (unsigned int i = 0; i < columns; ++i) {
        resultBinds[i].buffer_type = MYSQL_TYPE_STRING;
        resultBinds[i].buffer = buffers[i].data();
        resultBinds[i].buffer_length = kValueBuffer;
        resultBinds[i].length = &resultLengths[i];
        resultBinds[i].is_null = &resultNulls[i];
        resultBinds[i].error = &resultErrors[i];
    }

    if (mysql_stmt_bind_result(stmt, resultBinds.data()) != 0 ||
        mysql_stmt_store_result(stmt) != 0) {
        _lastErrorCode = mysql_stmt_errno(stmt);
        *error = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }

    int fetched = 0;
    while ((fetched = mysql_stmt_fetch(stmt)) == 0 || fetched == MYSQL_DATA_TRUNCATED) {
        ResultRow row;
        row.values.resize(columns);
        row.isNull.resize(columns);

        for (unsigned int i = 0; i < columns; ++i) {
            if (resultNulls[i] != 0) {
                row.isNull[i] = true;
                continue;
            }

            row.isNull[i] = false;

            if (resultLengths[i] <= kValueBuffer) {
                row.values[i].assign(buffers[i].data(), resultLengths[i]);
                continue;
            }

            // Значение не влезло — дочитываем целиком. Молча укороченное
            // значение хуже лишнего обращения к серверу.
            std::vector<char> big(resultLengths[i] + 1, '\0');
            MYSQL_BIND wide;
            std::memset(&wide, 0, sizeof(wide));
            wide.buffer_type = MYSQL_TYPE_STRING;
            wide.buffer = big.data();
            wide.buffer_length = resultLengths[i];

            if (mysql_stmt_fetch_column(stmt, &wide, i, 0) == 0) {
                row.values[i].assign(big.data(), resultLengths[i]);
            } else {
                row.values[i].assign(buffers[i].data(), kValueBuffer);
            }
        }

        rows->push_back(row);
    }

    mysql_stmt_close(stmt);
    return true;
}

bool MariaDatabase::Execute(const Statement& statement, int* affectedRows,
                            std::string* error) {
    return Run(statement, affectedRows, nullptr, error, true);
}

bool MariaDatabase::Query(const Statement& statement, std::vector<ResultRow>* rows,
                          std::string* error) {
    return Run(statement, nullptr, rows, error, true);
}

bool MariaDatabase::Ping(std::string* message) {
    std::string error;
    if (!Ensure(&error)) {
        *message = MaskSecrets(error);
        return false;
    }

    if (mysql_ping(_handle) != 0) {
        _lastErrorCode = mysql_errno(_handle);
        *message = MaskSecrets(mysql_error(_handle));

        // Одна попытка переподключиться: база могла перезагрузиться
        Close();
        if (!Ensure(&error)) {
            *message = MaskSecrets(error);
            return false;
        }
    }

    *message = std::string("MySQL ") + mysql_get_server_info(_handle);
    return true;
}

bool MariaDatabase::Begin(std::string* error) {
    if (!Ensure(error)) return false;

    if (mysql_autocommit(_handle, 0) != 0) {
        _lastErrorCode = mysql_errno(_handle);
        *error = mysql_error(_handle);
        return false;
    }

    _inTransaction = true;
    return true;
}

bool MariaDatabase::Commit(std::string* error) {
    if (_handle == nullptr) {
        *error = "нет соединения";
        return false;
    }

    const bool ok = mysql_commit(_handle) == 0;
    if (!ok) {
        _lastErrorCode = mysql_errno(_handle);
        *error = mysql_error(_handle);
    }

    mysql_autocommit(_handle, 1);
    _inTransaction = false;
    return ok;
}

void MariaDatabase::Rollback() {
    if (_handle == nullptr) return;

    mysql_rollback(_handle);
    mysql_autocommit(_handle, 1);
    _inTransaction = false;
}

}  // namespace ch
