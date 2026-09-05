// Превращение задания в запросы. Чистая функция: тексты и параметры собираются
// без единого обращения к базе, поэтому проверяются тестом без MySQL.
//
// Разделение не косметическое. Инварианты записи — идемпотентность закрытия
// сессии, «пустой адрес не затирает записанный», порядок агрегатов — живут именно
// здесь, и без такого разделения их пришлось бы проверять только на живом сервере.
#pragma once

#include "core/jobs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ch {

// Значение параметра. Идентификаторы (префикс таблиц) параметризовать нельзя —
// они проходят белый список; всё остальное уходит СЮДА и никогда не склеивается
// с текстом запроса.
struct SqlValue {
    enum class Kind { Null, Int64, UInt64, Text };

    Kind kind = Kind::Null;
    int64_t integer = 0;
    uint64_t unsignedInteger = 0;
    std::string text;

    static SqlValue Null() { return SqlValue{}; }
    static SqlValue Int(int64_t value);
    static SqlValue UInt(uint64_t value);
    static SqlValue Text(const std::string& value);

    // Значение, которого может не быть: NULL вместо пустой строки и нулей.
    static SqlValue OptionalText(bool present, const std::string& value);
    static SqlValue OptionalInt(bool present, int64_t value);
};

struct Statement {
    std::string sql;
    std::vector<SqlValue> params;
};

// Пустой адрес НЕ затирает уже записанный.
//
// Это корень проблемы «в базе 0.0.0.0, и правка руками не держится»: раньше адрес
// перезаписывался при КАЖДОМ старте, поэтому исправленная вручную строка
// возвращалась к мусору на следующем рестарте. Отсутствие адреса — это
// «нечего сказать», а не «сотри то, что есть».
std::string ServerUpsertSql(const std::string& prefix);

Statement BuildServerUpsert(const std::string& prefix, const WriteJob& job);

// Открытие сессии: INSERT IGNORE строки плюс upsert игрока.
// IGNORE, а не просто INSERT: задание может прийти повторно из спула,
// и дубль по session_key не должен превращаться в ошибку записи.
std::vector<Statement> BuildSessionOpen(const std::string& prefix, const WriteJob& job);

Statement BuildSessionCloseUpdate(const std::string& prefix, const WriteJob& job);
Statement BuildSessionExists(const std::string& prefix, const WriteJob& job);
Statement BuildSessionCloseInsert(const std::string& prefix, const WriteJob& job);
std::vector<Statement> BuildAggregates(const std::string& prefix, const WriteJob& job);

Statement BuildOnlineSnapshot(const std::string& prefix, const WriteJob& job);

// Решение о том, что делать после UPDATE закрытия.
//
// Закрытие сессии обязано быть идемпотентным: UPDATE ставит условие
// ended_at IS NULL, и если он ничего не изменил — сессия уже закрыта (задание
// пришло повторно из спула) или строки открытия вообще нет.
//
// Первый случай: выходим и НЕ трогаем счётчики, иначе повтор задания удваивал бы
// наигранное время. Второй: вставляем полную строку, чтобы данные не пропали совсем.
enum class CloseAction {
    // Ничего больше не делаем: сессия уже была закрыта
    AlreadyClosed,
    // UPDATE сработал — остаётся обновить агрегаты
    UpdateAggregates,
    // Строки не было — вставляем полную и обновляем агрегаты
    InsertThenAggregates,
};

CloseAction PlanClose(int affectedRows, bool rowExists);

}  // namespace ch
