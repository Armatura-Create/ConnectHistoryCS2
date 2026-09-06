using System;

namespace ConnectHistory.Tests;

/// Логгер-заглушка: тесты проверяют поведение, а не вывод в консоль.
internal sealed class NullLogger : ILogger
{
    public void Info(string message) { }
    public void Debug(string message) { }
    public void Warn(string message) { }
    public void Raw(string message) { }
    public void Error(string message, Exception? ex = null) { }
}
