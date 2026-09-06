using System;
using Microsoft.Extensions.Logging;

namespace ConnectHistory;

/// Логгер плагина поверх Core.Logger, уважающий Debug-флаг конфига.
///
/// Пишем через логгер SwiftlyS2, а не в Console: у фреймворка свои синки (консоль,
/// файл), и собственный Console.WriteLine прошёл бы мимо них и потерял метку плагина.
///
/// Флаг читается через замыкание, поэтому подхватывается после sw_ch_reload.
///
/// Сообщения объявлены через [LoggerMessage]: генератор разворачивает их в
/// закешированные делегаты, и анализаторы (CA1848/CA1873) видят, что аргументы
/// не вычисляются, когда уровень выключен.
public sealed partial class PluginLogger : ILogger
{
    private readonly Microsoft.Extensions.Logging.ILogger _sink;
    private readonly Func<bool> _isDebug;

    public PluginLogger(Microsoft.Extensions.Logging.ILogger sink, Func<bool> isDebug)
    {
        _sink = sink;
        _isDebug = isDebug;
    }

    [LoggerMessage(EventId = 1, Level = LogLevel.Information, Message = "[ConnectHistory] {Message}")]
    private static partial void WriteInfo(Microsoft.Extensions.Logging.ILogger logger, string message);

    [LoggerMessage(EventId = 2, Level = LogLevel.Warning, Message = "[ConnectHistory] {Message}")]
    private static partial void WriteWarn(Microsoft.Extensions.Logging.ILogger logger, string message);

    [LoggerMessage(EventId = 3, Level = LogLevel.Information, Message = "[ConnectHistory] [DEBUG] {Message}")]
    private static partial void WriteDebug(Microsoft.Extensions.Logging.ILogger logger, string message);

    [LoggerMessage(EventId = 6, Level = LogLevel.Information, Message = "{Message}")]
    private static partial void WriteRaw(Microsoft.Extensions.Logging.ILogger logger, string message);

    [LoggerMessage(EventId = 4, Level = LogLevel.Error, Message = "[ConnectHistory] {Message} => {Error}")]
    private static partial void WriteFailure(Microsoft.Extensions.Logging.ILogger logger, string message, string error);

    [LoggerMessage(EventId = 5, Level = LogLevel.Error, Message = "[ConnectHistory] {Message}")]
    private static partial void WriteFailure(Microsoft.Extensions.Logging.ILogger logger, string message);

    public void Info(string message) => WriteInfo(_sink, message);

    public void Warn(string message) => WriteWarn(_sink, message);

    // Без метки плагина: префикс на каждой строке разорвал бы рамку заставки
    public void Raw(string message) => WriteRaw(_sink, message);

    public void Debug(string message)
    {
        // Debug печатает SteamID, ники и IP игроков — по умолчанию выключен.
        // Собственный флаг, а не уровень логгера хоста: настройка плагина не должна
        // зависеть от того, как настроено логирование сервера.
        if (_isDebug()) WriteDebug(_sink, message);
    }

    public void Error(string message, Exception? ex = null)
    {
        // Текст исключения обязан быть очищен от секретов: MySqlConnector умеет
        // приложить строку подключения, а в ней пароль от боевой базы.
        // Стек кладём в тот же аргумент: отдельный параметр Exception логгер
        // напечатал бы целиком, вместе с неочищенным Message.
        if (ex != null)
            WriteFailure(_sink, message, SqlSanitizer.Mask(ex.Message) + "\nStack trace: " + ex.StackTrace);
        else
            WriteFailure(_sink, message);
    }
}
