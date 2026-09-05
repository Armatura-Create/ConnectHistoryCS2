using System;

namespace ConnectHistory;

/// Разбор значения DisplayTimeZone из конфига.
///
/// В базе время всегда UTC — это не обсуждается: сервер с локальной таймзоной иначе пишет
/// смешанные метки, и любой отчёт по часам врёт. Но игроку показывать UTC неудобно,
/// поэтому пояс отображения задаётся отдельно и на данные не влияет.
public static class TimeZoneResolver
{
    /// Пустое значение и "UTC" — UTC. "Local" — часовой пояс машины сервера.
    /// Всё остальное ищется среди системных: IANA ("Europe/Moscow") на Linux и macOS,
    /// на Windows работают и IANA, и windows-идентификаторы (ICU).
    ///
    /// Неизвестный пояс — не повод падать: громко предупреждаем и остаёмся на UTC.
    public static TimeZoneInfo Resolve(string? value, ILogger? logger = null)
    {
        if (string.IsNullOrWhiteSpace(value)) return TimeZoneInfo.Utc;

        var trimmed = value.Trim();

        if (trimmed.Equals("UTC", StringComparison.OrdinalIgnoreCase)) return TimeZoneInfo.Utc;
        if (trimmed.Equals("Local", StringComparison.OrdinalIgnoreCase)) return TimeZoneInfo.Local;

        try
        {
            return TimeZoneInfo.FindSystemTimeZoneById(trimmed);
        }
        catch (Exception ex) when (ex is TimeZoneNotFoundException or InvalidTimeZoneException)
        {
            logger?.Error($"[Config] DisplayTimeZone \"{trimmed}\" не найден в системе. " +
                          "Ожидается IANA-идентификатор (например Europe/Moscow), \"UTC\" или \"Local\". " +
                          "Игрокам время показывается в UTC");
            return TimeZoneInfo.Utc;
        }
    }
}
