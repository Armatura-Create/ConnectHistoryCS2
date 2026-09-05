using System;
using System.Globalization;
using SwiftlyS2.Shared.GameEventDefinitions;
using SwiftlyS2.Shared.Misc;
using SwiftlyS2.Shared.Players;
using SwiftlyS2.Shared.ProtobufDefinitions;

namespace ConnectHistory;

/// События игрока: открытие и закрытие сессии.
public sealed partial class ConnectHistory
{
    private HookResult OnPlayerConnectFull(EventPlayerConnectFull ev)
    {
        // UserIdPlayer может быть null у «призрачного» подключения — без проверки
        // обработчик падает прямо здесь.
        var player = ev.UserIdPlayer;
        if (player is null || player.IsFakeClient) return HookResult.Continue;

        OpenSessionFor(player);
        return HookResult.Continue;
    }

    private HookResult OnPlayerDisconnect(EventPlayerDisconnect ev)
    {
        // SteamID берём из поля события (XuID), а не с контроллера: на выходе игрока
        // контроллер уже может быть частично разобран, а поле события — обычное число,
        // скопированное движком в payload.
        var steamId = ev.XuID;

        if (!SteamIdUtil.IsRealSteamId(steamId))
        {
            var fallback = ev.UserIdPlayer;
            if (fallback is null) return HookResult.Continue;

            try
            {
                steamId = fallback.SteamID;
            }
            catch (Exception exception)
            {
                _logger.Debug($"[LEAVE] не удалось прочитать SteamID отключившегося игрока: {exception.Message}");
                return HookResult.Continue;
            }
        }

        var session = _sessions.Take(steamId);
        if (session == null) return HookResult.Continue;

        var reason = ev.Reason;
        CloseSession(session, ev.UserIdPlayer, SessionEndKind.Disconnect, reason, ReasonName(reason));

        _commandCooldown.Remove(steamId);
        return HookResult.Continue;
    }

    /// Раунды считаем событием, а не чтением схемы движка: нам нужно то, что игрок
    /// застал в ЭТОЙ сессии, а поля контроллера обнуляются сменой карты.
    private HookResult OnRoundEnd(EventRoundEnd ev)
    {
        foreach (var session in _sessions.Snapshot())
            session.NoteRoundEnd();

        return HookResult.Continue;
    }

    private HookResult OnPlayerTeam(EventPlayerTeam ev)
    {
        if (ev.IsBot) return HookResult.Continue;

        var player = ev.UserIdPlayer;
        if (player is null) return HookResult.Continue;

        if (_sessions.TryGet(player.SteamID, out var session))
            session.NoteTeam(ev.Team, DateTime.UtcNow);

        return HookResult.Continue;
    }

    /// Открывает сессию и СРАЗУ пишет строку в базу.
    ///
    /// Писать строку только на выходе нельзя: процесс игрового сервера умирает без
    /// предупреждения, и всё, что жило до этого момента только в памяти, исчезает
    /// вместе с ним. При записи на входе оборванная сессия остаётся видимым фактом
    /// (ended_at IS NULL), а «кто сейчас онлайн» — это обычный SELECT, без RCON и A2S.
    private void OpenSessionFor(IPlayer player)
    {
        ulong steamId;
        string nickname;

        try
        {
            steamId = player.SteamID;
            nickname = PluginText.Truncate(player.Name, PluginText.NicknameMaxLength);
        }
        catch (Exception ex)
        {
            _logger.Debug($"[JOIN] игрок невалиден, сессия не открыта: {ex.Message}");
            return;
        }

        if (!SteamIdUtil.IsRealSteamId(steamId))
        {
            _logger.Debug($"[JOIN] пропуск: SteamID {steamId.ToString(CultureInfo.InvariantCulture)} не похож на аккаунт Steam");
            return;
        }

        // Повторный player_connect_full без смены карты (реконнект в пределах карты)
        // не должен плодить открытые строки: старую закрываем как смену карты.
        var previous = _sessions.Take(steamId);
        if (previous != null)
            CloseSession(previous, player, SessionEndKind.MapChange, 0, nameof(SessionEndKind.MapChange));

        var ip = ReadPlayerIp(player);
        var geo = Config.Collect.GeoIp ? _geoIp.Lookup(ip) : default;

        var session = new OpenSession
        {
            Key = Guid.NewGuid().ToString("N"),
            SteamId64 = steamId,
            AccountId = SteamIdUtil.ToAccountId(steamId),
            StartedAt = DateTime.UtcNow,
            ConnectMap = CurrentMap(),
            Nickname = nickname,
            CountryIso = geo.Iso
        };

        // Отсчёт времени вне игры начинается вместе с сессией: до первого
        // player_team игрок выбирает команду, и это время тоже не игровое.
        session.StartTeamTracking(session.StartedAt);

        _sessions.Add(session);

        _writer?.Enqueue(new SessionOpenJob
        {
            SessionKey = session.Key,
            SteamId64 = session.SteamId64,
            AccountId = session.AccountId,
            ServerId = Config.ServerId,
            Nickname = session.Nickname,
            StartedAt = session.StartedAt,
            ConnectMap = session.ConnectMap,
            PlayersOnline = CountHumans(),
            MaxPlayers = SafeMaxPlayers(),
            ClientLang = ReadClientLanguage(player),
            PlayerIp = Config.Collect.PlayerIp ? NullIfEmpty(ip) : null,
            IpHash = Config.Collect.IpHash ? IpUtil.Hash(ip, Config.Collect.IpHashSalt) : null,
            IpSubnet = Config.Collect.PlayerIp || Config.Collect.IpHash ? IpUtil.ToSubnet(ip) : null,
            CountryIso = geo.Iso,
            CountryName = geo.Country,
            City = geo.City,
            PluginVersion = PluginVersion
        });

        _logger.Debug($"[JOIN] сессия {session.Key} открыта: {nickname} ({steamId}), карта {session.ConnectMap}");
    }

    /// Закрывает сессию. Игрок может быть уже невалиден — тогда итоги матча
    /// не снимутся, но сама сессия всё равно будет закрыта корректно.
    private void CloseSession(OpenSession session, IPlayer? player, SessionEndKind kind,
        int reason, string reasonName)
    {
        var endedAt = DateTime.UtcNow;
        var duration = (int)Math.Max(0, (endedAt - session.StartedAt).TotalSeconds);

        // Последний интервал команды закрывается здесь, иначе время после
        // последней смены команды нигде не учтётся.
        session.FinishTeamTracking(endedAt);

        // Наблюдательское время не может превышать саму сессию: часы сервера
        // могут прыгнуть, а отрицательное «наиграно» испортит агрегат навсегда.
        var spectator = Math.Clamp(session.SpectatorSeconds, 0, duration);

        if (player != null)
        {
            try
            {
                var name = PluginText.Truncate(player.Name, PluginText.NicknameMaxLength);
                if (!string.IsNullOrEmpty(name)) session.Nickname = name;
            }
            catch (Exception)
            {
                // Ник на выходе не критичен — остаётся тот, что был на входе
            }
        }

        var stats = Config.Collect.MatchStats ? StatsCollector.Collect(player, session, _logger) : null;

        _writer?.Enqueue(new SessionCloseJob
        {
            SessionKey = session.Key,
            SteamId64 = session.SteamId64,
            AccountId = session.AccountId,
            ServerId = Config.ServerId,
            Nickname = session.Nickname,
            StartedAt = session.StartedAt,
            EndedAt = endedAt,
            DurationSeconds = duration,
            SpectatorSeconds = spectator,
            CountSpectatorTime = Config.Collect.CountSpectatorTime,
            DisconnectMap = CurrentMap(),
            DisconnectReason = reason,
            DisconnectReasonName = PluginText.Truncate(reasonName, 64),
            EndKind = kind,
            Stats = stats,
            PingAvg = session.PingAvg,
            PingMin = session.PingMin,
            PingMax = session.PingMax,
            PingSamples = Config.Collect.Ping ? session.PingSamples : 0
        });

        _logger.Debug($"[LEAVE] сессия {session.Key} закрыта: {session.Nickname}, " +
                      $"{duration.ToString(CultureInfo.InvariantCulture)} с, причина {reasonName}");
    }

    /// Закрывает все открытые сессии одной причиной (смена карты, выгрузка плагина).
    /// Игроков здесь не ищем: на выгрузке плагина и на смене карты они уже
    /// могут быть недействительны.
    private void CloseAllSessions(SessionEndKind kind)
    {
        foreach (var session in _sessions.TakeAll())
            CloseSession(session, null, kind, 0, kind.ToString());
    }

    /// Открывает сессии всем, кто уже на сервере. Нужно после hot reload и sw_ch_reload.
    private void OpenSessionsForEveryone()
    {
        foreach (var player in Core.PlayerManager.GetAllPlayers())
        {
            if (player.IsFakeClient) continue;
            OpenSessionFor(player);
        }
    }

    private string? ReadPlayerIp(IPlayer player)
    {
        if (!Config.Collect.PlayerIp && !Config.Collect.IpHash && !Config.Collect.GeoIp) return null;

        try
        {
            return IpUtil.ExtractIp(player.IPAddress);
        }
        catch (Exception ex)
        {
            _logger.Debug($"[JOIN] IP игрока недоступен: {ex.Message}");
            return null;
        }
    }

    private string? ReadClientLanguage(IPlayer player)
    {
        try
        {
            // Language.Value — код языка ("en", "ru", "pt-BR"). В колонку идёт
            // двухбуквенная часть, как и в версии для CSSharp.
            var value = player.PlayerLanguage.Value;
            if (string.IsNullOrEmpty(value)) return null;

            var dash = value.IndexOf('-', StringComparison.Ordinal);
            return dash > 0 ? value[..dash] : value;
        }
        catch (Exception ex)
        {
            _logger.Debug($"[JOIN] язык клиента недоступен: {ex.Message}");
            return null;
        }
    }

    private int CountHumans()
    {
        var count = 0;
        foreach (var player in Core.PlayerManager.GetAllPlayers())
        {
            if (!player.IsFakeClient) count++;
        }

        return count;
    }

    private static string? NullIfEmpty(string? value) => string.IsNullOrEmpty(value) ? null : value;

    /// Имя причины отключения из Valve-шного перечисления, без префикса NETWORK_DISCONNECT_.
    /// Неизвестный код не теряем: он остаётся числом в disconnect_reason.
    internal static string ReasonName(int reason)
    {
        var name = Enum.IsDefined(typeof(ENetworkDisconnectionReason), reason)
            ? Enum.GetName(typeof(ENetworkDisconnectionReason), reason)
            : null;

        if (string.IsNullOrEmpty(name))
            return string.Create(CultureInfo.InvariantCulture, $"REASON_{reason}");

        const string prefix = "NETWORK_DISCONNECT_";
        return name.StartsWith(prefix, StringComparison.Ordinal) ? name[prefix.Length..] : name;
    }
}
