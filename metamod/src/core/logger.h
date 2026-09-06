// Логгер ядра. Ядро не знает ни про Metamod, ни про движок: наружу выставлен
// интерфейс, который в игре реализует вывод в консоль сервера, а в тестах — заглушка.
//
// Это единственный интерфейс с «одной реализацией» в проекте, и он оправдан:
// без него ни один инвариант ядра нельзя было бы проверить без запущенного CS2.
#pragma once

#include <string>

namespace ch {

class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void Info(const std::string& message) = 0;
    virtual void Warn(const std::string& message) = 0;
    virtual void Error(const std::string& message) = 0;

    // Debug печатает SteamID, ники и IP игроков — по умолчанию выключен.
    virtual void Debug(const std::string& message) = 0;

    // Строка без уровня. Нужна ровно одному потребителю — заставке при загрузке:
    // префикс на каждой строке разорвал бы рамку.
    virtual void Raw(const std::string& message) = 0;
};

// Логгер, который молчит. Нужен там, где логирование необязательно
// (санитайзер префикса вызывается и из тестов).
class NullLogger final : public ILogger {
public:
    void Info(const std::string&) override {}
    void Warn(const std::string&) override {}
    void Error(const std::string&) override {}
    void Debug(const std::string&) override {}
    void Raw(const std::string&) override {}
};

}  // namespace ch
