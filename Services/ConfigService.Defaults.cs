using System.Collections.Generic;
using System.IO;
using System.Text;

namespace ConnectHistory;

/// Значения по умолчанию и справочный README. Правишь дефолты — тебе сюда;
/// они применяются только при первом запуске (или после удаления Settings.json).
public sealed partial class ConfigService
{
    /// Дефолты обезличены сознательно: плагин ставят чужие люди, и первый запуск
    /// не имеет права попытаться подключиться к чужой базе.
    internal static SettingsConfig CreateDefaultSettings() => new()
    {
        Debug = false,
        ServerId = 1,
        DefaultLang = "RU",
        DisplayTimeZone = "UTC",
        Database = new DatabaseConfig
        {
            Host = "127.0.0.1",
            Port = 3306,
            Database = "connect_history",
            User = "",
            Password = "",
            SslMode = "Preferred",
            TablePrefix = "ch_",
            MaxPoolSize = 5,
            ConnectionTimeoutSeconds = 10,
            CommandTimeoutSeconds = 30
        },
        Collect = new CollectConfig(),
        Storage = new StorageConfig(),
        Commands = new CommandsConfig()
    };

    internal static MessagesConfig CreateDefaultMessages() => new()
    {
        Messages = new Dictionary<string, Dictionary<string, string>>
        {
            ["prefix"] = new()
            {
                ["RU"] = "{GREEN}[История]{DEFAULT} ",
                ["EN"] = "{GREEN}[History]{DEFAULT} "
            },
            ["playtime"] = new()
            {
                ["RU"] = "{prefix}Наиграно: {GOLD}{TOTAL}{DEFAULT}, заходов: {GOLD}{SESSIONS}{DEFAULT}, первый заход: {GOLD}{FIRST}",
                ["EN"] = "{prefix}Playtime: {GOLD}{TOTAL}{DEFAULT}, sessions: {GOLD}{SESSIONS}{DEFAULT}, first seen: {GOLD}{FIRST}"
            },
            ["playtime_current"] = new()
            {
                ["RU"] = "{prefix}Текущая сессия: {GOLD}{CURRENT}",
                ["EN"] = "{prefix}Current session: {GOLD}{CURRENT}"
            },
            ["lastseen_header"] = new()
            {
                ["RU"] = "{prefix}Последние заходы:",
                ["EN"] = "{prefix}Recent sessions:"
            },
            ["lastseen_row"] = new()
            {
                ["RU"] = " {GREY}{DATE}{DEFAULT} — {GOLD}{DURATION}{DEFAULT} на {LIGHTBLUE}{MAP}",
                ["EN"] = " {GREY}{DATE}{DEFAULT} — {GOLD}{DURATION}{DEFAULT} on {LIGHTBLUE}{MAP}"
            },
            ["no_data"] = new()
            {
                ["RU"] = "{prefix}{GREY}Данных пока нет",
                ["EN"] = "{prefix}{GREY}No data yet"
            },
            ["cooldown"] = new()
            {
                ["RU"] = "{prefix}{RED}Подождите {SECONDS} сек.",
                ["EN"] = "{prefix}{RED}Wait {SECONDS} sec."
            },
            ["db_error"] = new()
            {
                ["RU"] = "{prefix}{RED}База данных недоступна",
                ["EN"] = "{prefix}{RED}Database is unavailable"
            }
        }
    };

    /// README перезаписывается при каждой загрузке: пока он писался только при первом
    /// запуске, после обновления плагина он описывал старую версию.
    private static void WriteReadme(string directory)
    {
        var text = new StringBuilder()
            .AppendLine("ConnectHistory — история подключений и статистика игроков")
            .AppendLine("==========================================================")
            .AppendLine()
            .AppendLine("Файлы в этом каталоге:")
            .AppendLine("  Settings.json          — основные настройки (в нём пароль от БД!)")
            .AppendLine("  Messages.json          — тексты команд css_playtime / css_lastseen")
            .AppendLine("  *.schema.json          — JSON Schema, редактор подскажет поля и опечатки")
            .AppendLine()
            .AppendLine("ВАЖНО про безопасность")
            .AppendLine("  * Settings.json содержит пароль. Плагин выставляет ему права 600 (только владелец).")
            .AppendLine("  * Заведите для плагина ОТДЕЛЬНОГО пользователя MySQL с правами только")
            .AppendLine("    на его таблицы: SELECT, INSERT, UPDATE, CREATE, INDEX, ALTER.")
            .AppendLine("  * SslMode=Required, если база не на localhost: иначе ники, SteamID и IP")
            .AppendLine("    игроков идут по сети открытым текстом.")
            .AppendLine()
            .AppendLine("Основные настройки")
            .AppendLine("  ServerId               — номер сервера. У РАЗНЫХ серверов должен быть разным.")
            .AppendLine("  Debug                  — подробный лог (пишет SteamID, ники и IP игроков).")
            .AppendLine("  DisplayTimeZone        — пояс, в котором время видят ИГРОКИ:")
            .AppendLine("                           \"Europe/Moscow\", \"UTC\" или \"Local\".")
            .AppendLine("                           В базе время ВСЕГДА в UTC, эта настройка на неё не влияет.")
            .AppendLine("  Database.TablePrefix   — префикс таблиц, по умолчанию ch_")
            .AppendLine()
            .AppendLine("Что собирается (секция Collect)")
            .AppendLine("  GeoIp                  — страна и город по GeoLite2 (.mmdb рядом с DLL)")
            .AppendLine("  PlayerIp               — полный IP игрока (персональные данные)")
            .AppendLine("  IpHash + IpHashSalt    — HMAC от IP: мультиаккаунты видно, адрес не хранится.")
            .AppendLine("                           Пустая соль = хеширование выключено.")
            .AppendLine("  MatchStats             — убийства, смерти, урон, MVP, счёт")
            .AppendLine("  Ping                   — средний/мин/макс пинг за сессию")
            .AppendLine("  NicknameHistory        — таблица ch_nicknames")
            .AppendLine("  PlayerAggregates       — таблица ch_players (наиграно всего)")
            .AppendLine("  OnlineSnapshots        — точки графика посещаемости (ch_online_snapshots)")
            .AppendLine()
            .AppendLine("Если база недоступна")
            .AppendLine("  Записи копятся в pending-writes.jsonl рядом с DLL плагина и досылаются")
            .AppendLine("  автоматически. Storage.SpoolMaxEntries ограничивает размер файла.")
            .AppendLine()
            .AppendLine("Команды")
            .AppendLine("  css_ch_status   (@css/root)  — состояние: связь с БД, очередь, последняя ошибка")
            .AppendLine("  css_ch_reload   (@css/root)  — перечитать конфигурацию")
            .AppendLine("  css_playtime                 — своё наигранное время")
            .AppendLine("  css_lastseen                 — свои последние заходы")
            .AppendLine()
            .AppendLine("Полное описание схемы БД и готовые SQL-запросы — docs/DATABASE.md,")
            .AppendLine("модуль для панели Flute CMS — docs/FLUTE_MODULE.md.")
            .ToString();

        File.WriteAllText(Path.Combine(directory, "README.txt"), text, Encoding.UTF8);
    }
}
