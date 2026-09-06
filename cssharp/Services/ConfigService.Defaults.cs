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
        DefaultLang = "RU",
        DisplayTimeZone = "UTC",
        Server = new ServerConfig { Id = 1 },
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
            .AppendLine("ConnectHistory - connection history and player analytics")
            .AppendLine("========================================================")
            .AppendLine()
            .AppendLine("Files in this directory:")
            .AppendLine("  Settings.json          - main settings (contains the database password!)")
            .AppendLine("  Messages.json          - texts for the css_playtime / css_lastseen commands")
            .AppendLine("  *.schema.json          - JSON Schema; your editor will autocomplete and catch typos")
            .AppendLine()
            .AppendLine("SECURITY")
            .AppendLine("  * Settings.json holds a password. The plugin keeps it at mode 600 (owner only).")
            .AppendLine("  * Give the plugin its OWN MySQL user, limited to its tables:")
            .AppendLine("    SELECT, INSERT, UPDATE, CREATE, INDEX, ALTER.")
            .AppendLine("  * Use SslMode=Required when the database is not on localhost - otherwise")
            .AppendLine("    nicknames, SteamIDs and player IPs travel the network in clear text.")
            .AppendLine()
            .AppendLine("MAIN SETTINGS")
            .AppendLine("  Server.Id              - server number. MUST differ between servers.")
            .AppendLine("                           Was a top-level ServerId before 3.0.1; the old")
            .AppendLine("                           spelling is still read, so old configs keep working.")
            .AppendLine("  Debug                  - verbose log (prints SteamIDs, nicknames and player IPs).")
            .AppendLine("  DisplayTimeZone        - the time zone PLAYERS see times in:")
            .AppendLine("                           \"Europe/Moscow\", \"UTC\" or \"Local\".")
            .AppendLine("                           The database is ALWAYS UTC; this setting does not touch it.")
            .AppendLine("  Database.TablePrefix   - table prefix, ch_ by default")
            .AppendLine("  Server.PublicAddress   - public server address, \"ip:port\" or \"host:port\".")
            .AppendLine("                           The server process does not know its own public address:")
            .AppendLine("                           ConVar ip is the socket bind address, usually 0.0.0.0.")
            .AppendLine("                           Leave empty and the plugin will try to detect it, leaving")
            .AppendLine("                           the address column empty if it can only find 0.0.0.0")
            .AppendLine("                           or a private address.")
            .AppendLine()
            .AppendLine("WHAT IS COLLECTED (the Collect section)")
            .AppendLine("  GeoIp                  - country and city via GeoLite2 (.mmdb next to the plugin)")
            .AppendLine("  PlayerIp               - full player IP (personal data)")
            .AppendLine("  IpHash + IpHashSalt    - HMAC of the IP: multi-accounts stay visible, the address")
            .AppendLine("                           is not stored. An empty salt disables hashing.")
            .AppendLine("  MatchStats             - kills, deaths, damage, MVP, score")
            .AppendLine("  Ping                   - average/min/max ping per session")
            .AppendLine("  CountSpectatorTime     - count spectator time as playtime.")
            .AppendLine("                           false subtracts out-of-game time from the aggregate.")
            .AppendLine("                           The time itself always goes to spectator_seconds.")
            .AppendLine("  NicknameHistory        - the ch_nicknames table")
            .AppendLine("  PlayerAggregates       - the ch_players table (total playtime)")
            .AppendLine("  OnlineSnapshots        - attendance chart points (ch_online_snapshots)")
            .AppendLine()
            .AppendLine("IF THE DATABASE IS UNAVAILABLE")
            .AppendLine("  Records are spooled to pending-writes.jsonl next to the plugin and re-sent")
            .AppendLine("  automatically. Storage.SpoolMaxEntries caps the file size.")
            .AppendLine()
            .AppendLine("COMMANDS")
            .AppendLine("  css_ch_status   (@css/root)  - status: database link, queue, last error")
            .AppendLine("  css_ch_reload   (@css/root)  - reload the configuration")
            .AppendLine("  css_playtime                 - own playtime")
            .AppendLine("  css_lastseen                 - own recent sessions")
            .AppendLine()
            .AppendLine("Full database schema and a query cookbook: docs/DATABASE.md.")
            .AppendLine("Panel module: https://github.com/Armatura-Create/ConnectHistory-Flute")
            .ToString();

        File.WriteAllText(Path.Combine(directory, "README.txt"), text, Encoding.UTF8);
    }
}
