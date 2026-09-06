using System;

namespace ConnectHistory;

/// Простой интерфейс логгера для сервисов.
/// Сервисы не наследуют BasePlugin и не пишут в консоль напрямую.
public interface ILogger
{
    void Info(string message);
    void Debug(string message);
    void Warn(string message);
    void Error(string message, Exception? ex = null);

    /// Строка без метки времени и уровня. Нужна ровно одному потребителю —
    /// заставке при загрузке: префикс на каждой строке разорвал бы рамку.
    void Raw(string message);
}
