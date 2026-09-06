using System;
using System.Collections.Generic;
using System.Globalization;

namespace ConnectHistory;

/// Заставка при загрузке плагина.
///
/// Рамка считается, а не прибита пробелами: версия приходит из тега и может быть
/// длиннее ожидаемого ("3.0.0-rc1"), а разъехавшаяся рамка в консоли сервера —
/// первое, что видит о плагине его владелец.
public static class Banner
{
    private static readonly string[] Mark =
    [
        " ####   #   #",
        "#       #   #",
        "#       #####",
        "#       #   #",
        " ####   #   #",
    ];

    public static IReadOnlyList<string> Build(string version, string platform, int serverId)
    {
        var info = new[]
        {
            "ConnectHistory " + version,
            "Connection history and player analytics",
            "",
            "Platform : " + platform,
            "Server   : #" + serverId.ToString(CultureInfo.InvariantCulture),
        };

        var width = 0;
        foreach (var line in info) width = Math.Max(width, line.Length);

        info[2] = new string('-', width);

        var inner = 2 + Mark[0].Length + 3 + width + 2;
        var border = "+" + new string('-', inner) + "+";

        var lines = new List<string>(Mark.Length + 2) { border };
        for (var i = 0; i < Mark.Length; i++)
            lines.Add("|  " + Mark[i] + "   " + info[i].PadRight(width) + "  |");
        lines.Add(border);

        return lines;
    }
}
