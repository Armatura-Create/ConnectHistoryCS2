using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;
using System.Threading.Tasks;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Modules.Admin;
using CounterStrikeSharp.API.Modules.Commands;

namespace ConnectHistory;

/// Команды плагина.
///
/// Админские существуют, чтобы петля «что-то не пишется → в чём причина» была секундой,
/// а не чтением логов: css_ch_status показывает связь с базой, очередь и последнюю ошибку.
public sealed partial class ConnectHistory
{
    [RequiresPermissions("@css/root")]
    [ConsoleCommand("css_ch_status", "Состояние ConnectHistory: база, очередь, последняя ошибка")]
    public void OnStatusCommand(CCSPlayerController? caller, CommandInfo command)
    {
        var writer = _writer;
        var database = _database;

        command.ReplyToCommand($"[ConnectHistory] {ModuleVersion}");
        command.ReplyToCommand($"  Сервер #{Config.ServerId.ToString(CultureInfo.InvariantCulture)}, " +
                               $"открытых сессий: {_sessions.Count.ToString(CultureInfo.InvariantCulture)}");
        command.ReplyToCommand($"  База: {database?.Target ?? "не настроена"}");
        command.ReplyToCommand($"  Конфиг: {_configService.Directory}");

        if (writer == null)
        {
            command.ReplyToCommand("  Писатель не запущен");
            return;
        }

        command.ReplyToCommand($"  Очередь: {writer.Pending.ToString(CultureInfo.InvariantCulture)}, " +
                               $"записано: {writer.Written.ToString(CultureInfo.InvariantCulture)}, " +
                               $"в спуле: {writer.Spooled.ToString(CultureInfo.InvariantCulture)}" +
                               (writer.SpoolExists ? " (файл спула есть)" : ""));

        var lastError = writer.LastError;
        command.ReplyToCommand(lastError == null
            ? "  Последняя ошибка: нет"
            : $"  Последняя ошибка: {lastError}");

        if (database == null) return;

        // Проверка связи ходит по сети — только в фоне, ответ возвращаем в главный поток.
        var steamId = caller?.SteamID;
        _ = Task.Run(async () =>
        {
            var (ok, message) = await database.PingAsync(CancellationToken.None).ConfigureAwait(false);
            var text = ok ? $"  Связь с базой: OK ({message})" : $"  Связь с базой: ОШИБКА — {message}";

            Server.NextFrame(() =>
            {
                if (steamId == null)
                {
                    _logger.Info(text);
                    return;
                }

                var target = FindPlayer(steamId.Value);
                if (target != null) target.PrintToChat(text);
                else _logger.Info(text);
            });
        });
    }

    [RequiresPermissions("@css/root")]
    [ConsoleCommand("css_ch_reload", "Перечитать конфигурацию ConnectHistory")]
    public void OnReloadCommand(CCSPlayerController? caller, CommandInfo command)
    {
        // Открытые сессии закрываем ДО пересоздания писателя: иначе их закрытие уедет
        // в очередь, которую мы тут же выбросим.
        CloseAllSessions(SessionEndKind.PluginUnload);

        var old = _writer;
        _writer = null;

        if (old != null)
        {
            try
            {
                old.DisposeAsync().AsTask().GetAwaiter().GetResult();
            }
            catch (Exception ex)
            {
                _logger.Error("[Reload] Не удалось остановить старого писателя", ex);
            }
        }

        _pingTimer?.Kill();
        _snapshotTimer?.Kill();
        _pingTimer = null;
        _snapshotTimer = null;

        ReloadConfig();
        BuildDatabaseStack();
        StartTimers();

        // Игроки на сервере остались — открываем им сессии заново.
        foreach (var player in Utilities.GetPlayers())
        {
            if (player.IsBot || !player.IsValid) continue;
            OpenSessionFor(player);
        }

        command.ReplyToCommand("[ConnectHistory] Конфигурация перезагружена");
    }

    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    [ConsoleCommand("css_playtime", "Показать своё наигранное время")]
    public void OnPlaytimeCommand(CCSPlayerController? caller, CommandInfo command)
    {
        if (caller == null || !PlayerCommandAllowed(caller)) return;

        var steamId = caller.SteamID;
        var lang = ResolveLanguage(caller);
        var query = _query;
        if (query == null) return;

        // Текущая сессия известна нам и без базы — показываем сразу.
        if (_sessions.TryGet(steamId, out var open))
        {
            var current = (long)(DateTime.UtcNow - open.StartedAt).TotalSeconds;
            Reply(caller, lang, "playtime_current", new Dictionary<string, string>
            {
                ["{CURRENT}"] = ChatFormat.Duration(current)
            });
        }

        _ = Task.Run(async () =>
        {
            var totals = await query.GetTotalsAsync(steamId, CancellationToken.None).ConfigureAwait(false);

            Server.NextFrame(() =>
            {
                var target = FindPlayer(steamId);
                if (target == null) return;

                if (totals == null)
                {
                    Reply(target, lang, "no_data", null);
                    return;
                }

                Reply(target, lang, "playtime", new Dictionary<string, string>
                {
                    ["{TOTAL}"] = ChatFormat.Duration(totals.TotalSeconds),
                    ["{SESSIONS}"] = totals.Sessions.ToString(CultureInfo.InvariantCulture),
                    ["{FIRST}"] = ChatFormat.Date(totals.FirstSeen, _displayZone)
                });
            });
        });
    }

    [CommandHelper(whoCanExecute: CommandUsage.CLIENT_ONLY)]
    [ConsoleCommand("css_lastseen", "Показать свои последние заходы")]
    public void OnLastSeenCommand(CCSPlayerController? caller, CommandInfo command)
    {
        if (caller == null || !PlayerCommandAllowed(caller)) return;

        var steamId = caller.SteamID;
        var lang = ResolveLanguage(caller);
        var limit = Config.Commands.LastSeenLimit;
        var query = _query;
        if (query == null) return;

        _ = Task.Run(async () =>
        {
            var sessions = await query.GetRecentAsync(steamId, limit, CancellationToken.None).ConfigureAwait(false);

            Server.NextFrame(() =>
            {
                var target = FindPlayer(steamId);
                if (target == null) return;

                if (sessions == null || sessions.Count == 0)
                {
                    Reply(target, lang, "no_data", null);
                    return;
                }

                Reply(target, lang, "lastseen_header", null);

                foreach (var session in sessions)
                {
                    Reply(target, lang, "lastseen_row", new Dictionary<string, string>
                    {
                        ["{DATE}"] = ChatFormat.Date(session.StartedAt, _displayZone),
                        ["{DURATION}"] = ChatFormat.Duration(session.DurationSeconds),
                        ["{MAP}"] = session.Map
                    });
                }
            });
        });
    }

    // ---- Вспомогательное -------------------------------------------------------

    /// Команды игроков ходят в базу, поэтому у них кулдаун. Словарь чистится
    /// в EventPlayerDisconnect — иначе он растёт всё время жизни сервера.
    private bool PlayerCommandAllowed(CCSPlayerController player)
    {
        if (!Config.Commands.PlayerCommandsEnabled) return false;

        var cooldown = Math.Max(0, Config.Commands.CooldownSeconds);
        if (cooldown == 0) return true;

        var steamId = player.SteamID;
        var now = DateTime.UtcNow;

        if (_commandCooldown.TryGetValue(steamId, out var next) && next > now)
        {
            var left = (int)Math.Ceiling((next - now).TotalSeconds);
            Reply(player, ResolveLanguage(player), "cooldown", new Dictionary<string, string>
            {
                ["{SECONDS}"] = left.ToString(CultureInfo.InvariantCulture)
            });
            return false;
        }

        _commandCooldown[steamId] = now.AddSeconds(cooldown);
        return true;
    }

    private string ResolveLanguage(CCSPlayerController player)
    {
        try
        {
            var language = CounterStrikeSharp.API.Core.Translations.PlayerLanguageExtensions
                .GetLanguage(player)?.TwoLetterISOLanguageName;
            return string.IsNullOrEmpty(language) ? Config.DefaultLang : language;
        }
        catch (Exception)
        {
            return Config.DefaultLang;
        }
    }

    private void Reply(CCSPlayerController player, string lang, string key, IReadOnlyDictionary<string, string>? values)
    {
        var template = Localize(key, lang);
        if (string.IsNullOrEmpty(template)) return;

        var merged = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["{prefix}"] = Localize("prefix", lang)
        };

        if (values != null)
        {
            foreach (var (k, v) in values) merged[k] = v;
        }

        try
        {
            // EnsureChatColorPrefix обязателен: движок съедает цветовой код, стоящий
            // в самом начале сообщения, и строка вида "{GREEN}[История] …" выходит белой.
            player.PrintToChat(ChatFormat.EnsureChatColorPrefix(ChatFormat.Render(template, merged)));
        }
        catch (Exception ex)
        {
            // PrintTo* бросает, если сущность стала невалидной — пропустить одного
            // получателя дешевле, чем сорвать обработчик команды.
            _logger.Debug($"[CMD] не удалось отправить сообщение: {ex.Message}");
        }
    }

    /// Язык игрока -> язык по умолчанию -> первый доступный перевод.
    /// Пустая строка означает «ключа нет вообще» — тогда просто молчим.
    private string Localize(string key, string lang)
    {
        if (!Config.Messages.TryGetValue(key, out var translations) || translations.Count == 0)
            return string.Empty;

        if (translations.TryGetValue(lang, out var text)) return text;
        if (translations.TryGetValue(Config.DefaultLang, out text)) return text;

        foreach (var value in translations.Values) return value;
        return string.Empty;
    }

    /// Игрока ищем заново по SteamID: между уходом задачи в фон и её ответом игрок
    /// мог выйти, а объект контроллера — освободиться.
    private static CCSPlayerController? FindPlayer(ulong steamId)
    {
        foreach (var player in Utilities.GetPlayers())
        {
            if (!player.IsBot && player.SteamID == steamId) return player;
        }

        return null;
    }
}
