#include "core/schema_service.h"

#include "core/database.h"
#include "core/logger.h"
#include "core/schema_sql.h"
#include "core/util/sql_sanitizer.h"
#include "core/util/timeutil.h"

#include <cstdlib>
#include <vector>

namespace ch {
namespace {

Statement Plain(const std::string& sql) {
    Statement statement;
    statement.sql = sql;
    return statement;
}

}  // namespace

bool SchemaService::EnsureSchema() {
    const std::string prefix = _database->Prefix();
    std::string error;

    for (const std::string& ddl : BuildSchema(prefix)) {
        if (!_database->Execute(Plain(ddl), nullptr, &error)) {
            if (_logger != nullptr) {
                _logger->Error("[DB] Не удалось создать/проверить схему. Плагин продолжит "
                               "работу, но записи будут копиться в спуле до восстановления "
                               "базы: " + MaskSecrets(error));
            }
            return false;
        }
    }

    int version = ReadVersion();

    if (version > kSchemaVersion) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Схема в базе версии " + std::to_string(version) +
                           ", плагин знает только " + std::to_string(kSchemaVersion) +
                           ". Обновите плагин — старая версия может не понимать "
                           "новые колонки");
        }
        return true;
    }

    for (const MigrationStep& step : Migrations(prefix)) {
        if (step.target <= version) continue;

        if (_logger != nullptr) {
            _logger->Info("[DB] Миграция схемы " + std::to_string(version) + " -> " +
                          std::to_string(step.target));
        }

        if (!ApplyMigration(step.sql)) return false;
        version = step.target;
    }

    Statement writeVersion;
    writeVersion.sql = "INSERT INTO `" + prefix +
                       "schema_version` (`k`, `v`, `updated_at`) VALUES ('schema', ?, ?) "
                       "ON DUPLICATE KEY UPDATE `v` = VALUES(`v`), "
                       "`updated_at` = VALUES(`updated_at`)";
    writeVersion.params = {
        SqlValue::Int(kSchemaVersion),
        SqlValue::Text(FormatSqlDateTime(UtcNowSeconds())),
    };

    if (!_database->Execute(writeVersion, nullptr, &error)) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Не удалось записать версию схемы: " + MaskSecrets(error));
        }
        return false;
    }

    if (_logger != nullptr) {
        _logger->Info("[DB] Схема готова (версия " + std::to_string(kSchemaVersion) + ")");
    }
    return true;
}

// Шаг миграции, устойчивый к повторному применению.
//
// Правило то же, что и у BuildSchema: DDL идемпотентен. Но у ALTER нет формы
// IF NOT EXISTS в MySQL, поэтому «объект уже существует» — признак «шаг применён»,
// а не сбой. Все прочие ошибки всплывают: молча проглоченная миграция страшнее
// упавшей — вторая видна сразу, первая всплывёт неверными данными.
bool SchemaService::ApplyMigration(const std::string& sql) {
    std::string error;
    if (_database->Execute(Plain(sql), nullptr, &error)) return true;

    if (IsAlreadyAppliedError(_database->LastErrorCode())) {
        if (_logger != nullptr) {
            _logger->Info("[DB] Шаг миграции уже применён, пропускаю: " + MaskSecrets(error));
        }
        return true;
    }

    if (_logger != nullptr) {
        _logger->Error("[DB] Шаг миграции не выполнен: " + MaskSecrets(error));
    }
    return false;
}

int SchemaService::ReadVersion() {
    Statement statement;
    statement.sql = "SELECT `v` FROM `" + _database->Prefix() +
                    "schema_version` WHERE `k` = 'schema'";

    std::vector<ResultRow> rows;
    std::string error;
    if (!_database->Query(statement, &rows, &error)) return 0;
    if (rows.empty() || rows[0].values.empty()) return 0;
    if (!rows[0].isNull.empty() && rows[0].isNull[0]) return 0;

    return std::atoi(rows[0].values[0].c_str());
}

int SchemaService::MarkStaleSessions(int serverId) {
    Statement statement;
    statement.sql = "UPDATE `" + _database->Prefix() +
                    "sessions` SET `end_kind` = ? "
                    "WHERE `server_id` = ? AND `ended_at` IS NULL AND `end_kind` = 0";
    statement.params = {
        SqlValue::Int(static_cast<int64_t>(SessionEndKind::Stale)),
        SqlValue::Int(serverId),
    };

    int affected = 0;
    std::string error;
    if (!_database->Execute(statement, &affected, &error)) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Не удалось пометить незакрытые сессии: " + MaskSecrets(error));
        }
        return 0;
    }

    if (affected > 0 && _logger != nullptr) {
        _logger->Warn("[DB] Найдено " + std::to_string(affected) +
                      " незакрытых сессий от прошлого запуска — сервер завершился "
                      "аварийно. Помечены как stale");
    }
    return affected;
}

}  // namespace ch
