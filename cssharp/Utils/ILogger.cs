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
}
