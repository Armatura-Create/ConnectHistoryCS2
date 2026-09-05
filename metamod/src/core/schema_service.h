// Создание и миграция схемы. Работает через IDatabase, поэтому вся логика —
// порядок DDL, чтение версии, обработка «шаг уже применён» — проверяется
// заглушкой, без живого MySQL.
#pragma once

#include <string>

namespace ch {

class IDatabase;
class ILogger;

class SchemaService {
public:
    SchemaService(IDatabase* database, ILogger* logger)
        : _database(database), _logger(logger) {}

    // false — схему подготовить не удалось. Это НЕ повод останавливать плагин:
    // записи будут копиться в спуле до восстановления базы.
    bool EnsureSchema();

    // Сессии, оставшиеся открытыми от прошлого запуска ЭТОГО сервера, — следы
    // аварийного завершения процесса. Мы их не удаляем и не выдумываем им время
    // выхода: помечаем end_kind = Stale, чтобы «сейчас онлайн» считалось верно,
    // а карта падений сервера осталась в данных.
    int MarkStaleSessions(int serverId);

private:
    bool ApplyMigration(const std::string& sql);
    int ReadVersion();

    IDatabase* _database;
    ILogger* _logger;
};

}  // namespace ch
