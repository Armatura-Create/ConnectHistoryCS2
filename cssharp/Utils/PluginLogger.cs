using System;
using System.Globalization;

namespace ConnectHistory;

/// Логгер плагина, уважающий Debug-флаг конфига.
/// Флаг читается через замыкание, поэтому подхватывается после css_ch_reload.
public sealed class PluginLogger : ILogger
{
    private readonly Func<bool> _isDebug;

    public PluginLogger(Func<bool> isDebug)
    {
        _isDebug = isDebug;
    }

    private static string Stamp() => DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture);

    public void Info(string message) => Console.WriteLine($"[{Stamp()}] [ConnectHistory] [INFO] {message}");

    public void Warn(string message) => Console.WriteLine($"[{Stamp()}] [ConnectHistory] [WARN] {message}");

    public void Debug(string message)
    {
        if (_isDebug())
            Console.WriteLine($"[{Stamp()}] [ConnectHistory] [DEBUG] {message}");
    }

    public void Raw(string message) => Console.WriteLine(message);

    public void Error(string message, Exception? ex = null)
    {
        // Текст исключения обязан быть очищен от секретов: MySqlConnector умеет
        // приложить строку подключения, а в ней пароль от боевой базы.
        if (ex != null)
            Console.WriteLine($"[{Stamp()}] [ConnectHistory] [ERROR] {message} => " +
                              $"{SqlSanitizer.Mask(ex.Message)}\nStack trace: {ex.StackTrace}");
        else
            Console.WriteLine($"[{Stamp()}] [ConnectHistory] [ERROR] {message}");
    }
}
