using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;
using System.Threading.Tasks;
using SwiftlyS2.Shared.Commands;
using SwiftlyS2.Shared.Players;

namespace ConnectHistory;

/// Команды плагина.
///
/// Админские существуют, чтобы петля «что-то не пишется → в чём причина» была секундой,
/// а не чтением логов: sw_ch_status показывает связь с базой, очередь и последнюю ошибку.
///
/// Регистрируем не атрибутами, а вручную: команды нужно снимать в Unload, иначе после
/// выгрузки плагина в консоли остаются висеть их имена.
public sealed partial class ConnectHistory
{
    /// Право на админские команды. В CSSharp это было "@css/root".
    private const string AdminPermission = "connecthistory.admin";

    private readonly List<Guid> _commands = [];

    private void RegisterCommands()
    {
        _commands.Add(Core.Command.RegisterCommand("ch_status", OnStatusCommand,
            registerRaw: false, permission: AdminPermission,
            helpText: "Состояние ConnectHistory: база, очередь, последняя ошибка"));

        _commands.Add(Core.Command.RegisterCommand("ch_reload", OnReloadCommand,
            registerRaw: false, permission: AdminPermission,
            helpText: "Перечитать конфигурацию ConnectHistory"));

        _commands.Add(Core.Command.RegisterCommand("playtime", OnPlaytimeCommand,
            registerRaw: false, permission: "",
            helpText: "Показать своё наигранное время"));

        _commands.Add(Core.Command.RegisterCommand("lastseen", OnLastSeenCommand,
            registerRaw: false, permission: "",
            helpText: "Показать свои последние заходы"));
    }

    private void UnregisterCommands()
    {
        foreach (var guid in _commands) Core.Command.UnregisterCommand(guid);
        _commands.Clear();
    }

    private void OnStatusCommand(ICommandContext context)
    {
        var writer = _writer;
        var database = _database;

        context.Reply($"[ConnectHistory] {PluginVersion}");
        context.Reply($"  Сервер #{Config.ServerId.ToString(CultureInfo.InvariantCulture)}, " +
                      $"открытых сессий: {_sessions.Count.ToString(CultureInfo.InvariantCulture)}");
        context.Reply($"  База: {database?.Target ?? "не настроена"}");
        context.Reply($"  Конфиг: {_configService.Directory}");

        if (writer == null)
        {
            context.Reply("  Писатель не запущен");
            return;
        }

        context.Reply($"  Очередь: {writer.Pending.ToString(CultureInfo.InvariantCulture)}, " +
                      $"записано: {writer.Written.ToString(CultureInfo.InvariantCulture)}, " +
                      $"в спуле: {writer.Spooled.ToString(CultureInfo.InvariantCulture)}" +
                      (writer.SpoolExists ? " (файл спула есть)" : ""));

        var lastError = writer.LastError;
        context.Reply(lastError == null
            ? "  Последняя ошибка: нет"
            : $"  Последняя ошибка: {lastError}");

        if (database == null) return;

        // Проверка связи ходит по сети — только в фоне, ответ возвращаем в главный поток.
        // Через границу кадра проносим SteamID, а не IPlayer: за время запроса игрок
        // успевает выйти, и объект становится чужой памятью.
        var steamId = context.IsSentByPlayer ? context.Sender?.SteamID : null;
        _ = Task.Run(async () =>
        {
            var (ok, message) = await database.PingAsync(CancellationToken.None).ConfigureAwait(false);
            var text = ok ? $"  Связь с базой: OK ({message})" : $"  Связь с базой: ОШИБКА — {message}";

            Core.Scheduler.NextTick(() =>
            {
                if (steamId == null)
                {
                    _logger.Info(text);
                    return;
                }

                var target = FindPlayer(steamId.Value);
                if (target != null) SendChat(target, text);
                else _logger.Info(text);
            });
        });
    }

    private void OnReloadCommand(ICommandContext context)
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

        StopTimers();

        ReloadConfig();
        BuildDatabaseStack();
        StartTimers();

        // Игроки на сервере остались — открываем им сессии заново.
        OpenSessionsForEveryone();

        context.Reply("[ConnectHistory] Конфигурация перезагружена");
    }

    private void OnPlaytimeCommand(ICommandContext context)
    {
        if (!context.IsSentByPlayer) return;

        var caller = context.Sender;
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

            Core.Scheduler.NextTick(() =>
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

    private void OnLastSeenCommand(ICommandContext context)
    {
        if (!context.IsSentByPlayer) return;

        var caller = context.Sender;
        if (caller == null || !PlayerCommandAllowed(caller)) return;

        var steamId = caller.SteamID;
        var lang = ResolveLanguage(caller);
        var limit = Config.Commands.LastSeenLimit;
        var query = _query;
        if (query == null) return;

        _ = Task.Run(async () =>
        {
            var sessions = await query.GetRecentAsync(steamId, limit, CancellationToken.None).ConfigureAwait(false);

            Core.Scheduler.NextTick(() =>
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
    /// в player_disconnect — иначе он растёт всё время жизни сервера.
    private bool PlayerCommandAllowed(IPlayer player)
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

    private string ResolveLanguage(IPlayer player)
    {
        try
        {
            var value = player.PlayerLanguage.Value;
            if (string.IsNullOrEmpty(value)) return Config.DefaultLang;

            var dash = value.IndexOf('-', StringComparison.Ordinal);
            return dash > 0 ? value[..dash] : value;
        }
        catch (Exception)
        {
            return Config.DefaultLang;
        }
    }

    private void Reply(IPlayer player, string lang, string key, IReadOnlyDictionary<string, string>? values)
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

        // EnsureChatColorPrefix обязателен: движок съедает цветовой код, стоящий
        // в самом начале сообщения, и строка вида "{GREEN}[История] …" выходит белой.
        SendChat(player, ChatFormat.EnsureChatColorPrefix(ChatFormat.Render(template, merged)));
    }

    /// SendChat помечен [ThreadUnsafe] и зовётся только из главного потока —
    /// все вызовы сюда приходят либо прямо из обработчика команды, либо из NextTick.
    private void SendChat(IPlayer player, string message)
    {
        try
        {
            player.SendChat(message);
        }
        catch (Exception ex)
        {
            // Send* бросает, если игрок стал невалидным — пропустить одного
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
    /// мог выйти, а объект — освободиться.
    private IPlayer? FindPlayer(ulong steamId)
    {
        try
        {
            return Core.PlayerManager.GetPlayerFromSteamId(steamId, allowUnauthorized: false);
        }
        catch (Exception)
        {
            return null;
        }
    }
}
