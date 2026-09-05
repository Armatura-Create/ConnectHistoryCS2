// Доступ к базе, каким его видит ядро.
//
// Интерфейс с двумя реализациями: настоящий клиент MariaDB и заглушка в тестах.
// Без него самая опасная часть плагина — писатель с его ретраями, спулом и
// идемпотентным закрытием сессии — проверялась бы только на живом сервере,
// а именно там ошибка стоит дороже всего.
#pragma once

#include "core/sql_builder.h"

#include <string>
#include <vector>

namespace ch {

// Строка результата: значения как строки, NULL отдельным флагом.
// Читаем мы немного (наиграно, последние заходы), и типизировать нечего.
struct ResultRow {
    std::vector<std::string> values;
    std::vector<bool> isNull;
};

class IDatabase {
public:
    virtual ~IDatabase() = default;

    // Куда мы подключаемся — строка для логов БЕЗ пароля.
    virtual std::string Target() const = 0;

    // Префикс таблиц, уже прошедший белый список.
    virtual std::string Prefix() const = 0;

    // Соединение поднимается лениво и переустанавливается после обрыва.
    virtual bool Ping(std::string* message) = 0;

    // affectedRows может быть nullptr, если он не нужен.
    virtual bool Execute(const Statement& statement, int* affectedRows,
                         std::string* error) = 0;

    virtual bool Query(const Statement& statement, std::vector<ResultRow>* rows,
                       std::string* error) = 0;

    virtual bool Begin(std::string* error) = 0;
    virtual bool Commit(std::string* error) = 0;
    virtual void Rollback() = 0;

    // Код ошибки последнего запроса — нужен миграциям, чтобы отличить
    // «объект уже существует» от настоящего сбоя.
    virtual unsigned int LastErrorCode() const = 0;
};

}  // namespace ch
