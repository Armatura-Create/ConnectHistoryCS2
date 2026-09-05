using System.Text.RegularExpressions;

namespace ConnectHistory;

/// Вычищает секреты из строк, которые попадают в лог.
///
/// Причина простая: строка подключения собирается из конфига, а исключения MySqlConnector
/// и .NET регулярно включают её в текст. Один такой ERROR в консоли сервера — и пароль
/// от боевой базы лежит в логах, которые пересылают в чат поддержки.
public static partial class SqlSanitizer
{
    private const string Replacement = "***";

    // password=... / pwd=... до ближайшего ';' или конца строки
    [GeneratedRegex(@"(?i)\b(password|pwd)\s*=\s*[^;]*", RegexOptions.CultureInvariant)]
    private static partial Regex PasswordRegex();

    /// Заменяет значение пароля в любой строке (сообщение исключения, строка подключения).
    public static string Mask(string? text)
        => string.IsNullOrEmpty(text) ? string.Empty : PasswordRegex().Replace(text, $"password={Replacement}");
}
