// Спул: задания, которые база не приняла, ложатся на диск и досылаются позже.
//
// Это единственная защита от потери данных при недоступной базе: процесс игрового
// сервера умирает без предупреждения, и всё, что жило только в памяти, исчезает.
//
// Формат — JSON Lines. Сериализация вынесена отдельно от файловых операций,
// чтобы круговорот «задание -> строка -> задание» проверялся тестом: молча
// потерянное при разборе поле обнаружилось бы пустой колонкой месяцы спустя.
#pragma once

#include "core/jobs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ch {

class ILogger;

// Одна строка JSONL. Переводов строки внутри не бывает — их экранирует JSON.
std::string SerializeJob(const WriteJob& job);

// false — строка не разобрана (битый файл, чужой формат). Такую пропускают.
bool DeserializeJob(const std::string& line, WriteJob* job);

// Файл спула. Все операции терпимы к ошибкам: невозможность записать спул
// не должна ронять писателя, у которого и так проблемы с базой.
class Spool {
public:
    Spool(const std::string& path, int32_t maxEntries, ILogger* logger)
        : _path(path), _maxEntries(maxEntries), _logger(logger) {}

    const std::string& Path() const { return _path; }
    bool Exists() const;

    // Добавляет задание. false — не записали (предел, ошибка ввода-вывода).
    bool Append(const WriteJob& job);

    // Читает файл целиком и УДАЛЯЕТ его до отправки: если запись снова не пройдёт,
    // задания вернутся в спул обычным путём, а не задвоятся.
    std::vector<WriteJob> TakeAll();

private:
    int32_t CountLines() const;

    std::string _path;
    int32_t _maxEntries;
    ILogger* _logger;
};

}  // namespace ch
