using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;

namespace ConnectHistory;

/// Подстановка цветовых тегов и значений в шаблоны из Messages.json.
///
/// Управляющие байты выписаны здесь явно, а не взяты из хелпера SwiftlyS2: теги
/// в Messages.json — часть контракта с админом сервера, и один и тот же файл должен
/// одинаково работать под обеими целями. Значения сверены с таблицами
/// CounterStrikeSharp.ChatColors и SwiftlyS2.Helper — они совпадают до байта,
/// это коды самого движка CS2, а не изобретение фреймворка.
public static partial class ChatFormat
{
    private const char Default = '\x01';

    private static readonly (string Tag, char Color)[] Tags =
    [
        ("{DEFAULT}", Default),
        ("{LIGHTBLUE}", '\x0B'),
        ("{GREEN}", '\x04'),
        ("{GOLD}", '\x10'),
        ("{GREY}", '\x08'),
        ("{RED}", '\x07'),
        ("{BLUE}", '\x0B'),
        ("{PURPLE}", '\x0E'),
        ("{ORANGE}", '\x10'),
        ("{OLIVE}", '\x05')
    ];

    [GeneratedRegex("[\x01-\x10]", RegexOptions.CultureInvariant)]
    private static partial Regex ColorCodesRegex();

    /// Значения подставляются ДО цветов: сами значения тегов не содержат, а вот
    /// шаблон вокруг них — содержит.
    public static string Render(string template, IReadOnlyDictionary<string, string>? values = null)
    {
        if (string.IsNullOrEmpty(template)) return string.Empty;

        var result = template;

        if (values != null)
        {
            foreach (var (key, value) in values)
                result = result.Replace(key, value, StringComparison.OrdinalIgnoreCase);
        }

        foreach (var (tag, color) in Tags)
            result = result.Replace(tag, color.ToString(), StringComparison.OrdinalIgnoreCase);

        return result;
    }

    /// Приводит строку для чата к виду "\x01 " + текст.
    ///
    /// Движок CS2 не применяет цвет, стоящий в САМОМ НАЧАЛЕ сообщения: первый цветовой код
    /// съедается, и строка выходит белой. Ровно поэтому и SwiftlyS2 в своём Colored()
    /// приписывает пробел строке, начинающейся с тега.
    ///
    /// Ставятся обе части: код цвета по умолчанию закрывает случай «съедается первый код»,
    /// пробел — случай «перед первым кодом нужен обычный символ». Какая из двух моделей
    /// поведения движка верна, снаружи не различить, а сочетание работает в любой.
    ///
    /// Соблазнительная «оптимизация» — ранний выход, если строка уже начинается с кода
    /// цвета, — отключает починку ровно в том единственном случае, когда она нужна:
    /// шаблон "{GREEN}[История]" выводился бы белым, а тот же шаблон с пробелом в начале —
    /// цветным. Так это и ломалось в NotifyMessages.
    ///
    /// Функция идемпотентна: повторный вызов не плодит ни кодов, ни пробелов.
    public static string EnsureChatColorPrefix(string input)
    {
        if (string.IsNullOrEmpty(input)) return input;

        // Без цветов чинить нечего — не приписываем пробел обычному тексту
        if (!ColorCodesRegex().IsMatch(input)) return input;

        var body = input;

        // Снимаем то, что могли поставить сами или что уже есть в шаблоне,
        // чтобы результат не зависел от того, сколько раз сюда зашли
        if (body[0] == Default) body = body[1..];
        if (body.Length > 0 && body[0] == ' ') body = body[1..];

        return Default + " " + body;
    }

    /// Человекочитаемая длительность: "3h 12m" вместо 11520 секунд.
    /// Числа форматируются через InvariantCulture — сервер с арабской или турецкой
    /// локалью иначе показывает игрокам другие цифры.
    public static string Duration(long seconds)
    {
        if (seconds < 0) seconds = 0;

        var time = TimeSpan.FromSeconds(seconds);
        var days = (int)time.TotalDays;

        if (days > 0)
            return string.Create(CultureInfo.InvariantCulture, $"{days}d {time.Hours}h {time.Minutes}m");
        if (time.Hours > 0)
            return string.Create(CultureInfo.InvariantCulture, $"{time.Hours}h {time.Minutes}m");
        if (time.Minutes > 0)
            return string.Create(CultureInfo.InvariantCulture, $"{time.Minutes}m {time.Seconds}s");

        return string.Create(CultureInfo.InvariantCulture, $"{time.Seconds}s");
    }

    /// Дата для игрока: в базе всё в UTC, а человеку показываем в часовом поясе
    /// из Settings.json (DisplayTimeZone).
    public static string Date(DateTime utc, TimeZoneInfo? zone = null)
    {
        // Kind приходит из базы: MySqlConnector помечает значения как Utc
        // (DateTimeKind=Utc в строке подключения). Unspecified трактуем как UTC —
        // именно в нём плагин пишет.
        var source = utc.Kind == DateTimeKind.Unspecified
            ? DateTime.SpecifyKind(utc, DateTimeKind.Utc)
            : utc.ToUniversalTime();

        var local = zone == null || zone == TimeZoneInfo.Utc
            ? source
            : TimeZoneInfo.ConvertTimeFromUtc(source, zone);

        return local.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
    }
}
