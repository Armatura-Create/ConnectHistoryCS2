using System;
using System.Globalization;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.ValveConstants.Protobuf;

namespace ConnectHistory;

/// События игрока: открытие и закрытие сессии.
public sealed partial class ConnectHistory
{
    private HookResult OnPlayerConnectFull(EventPlayerConnectFull ev, GameEventInfo info)
    {
        // Userid у события nullable: без проверки «призрачное» подключение
        // даёт NullReferenceException прямо в обработчике.
        var player = ev.Userid;
        if (player is null || !player.IsValid || player.IsBot) return HookResult.Continue;

        OpenSessionFor(player);
        return HookResult.Continue;
    }

    private HookResult OnPlayerDisconnect(EventPlayerDisconnect ev, GameEventInfo info)
    {
        var player = ev.Userid;
        if (player is null || player.IsBot) return HookResult.Continue;

        // IsValid здесь НЕ проверяем: контроллер уже может быть частично разобран,
        // но сессию закрыть всё равно обязаны. Чтение полей защищено ниже.
        ulong steamId;
        try
        {
            steamId = player.SteamID;
        }
        catch (Exception ex)
        {
            _logger.Debug($"[LEAVE] не удалось прочитать SteamID отключившегося игрока: {ex.Message}");
            return HookResult.Continue;
        }

        var session = _sessions.Take(steamId);
        if (session == null) return HookResult.Continue;

        var reason = ev.Reason;
        CloseSession(session, player, SessionEndKind.Disconnect, reason, ReasonName(reason));

        _commandCooldown.Remove(steamId);
        return HookResult.Continue;
    }

    /// Раунды считаем событием, а не чтением схемы движка: нам нужно то, что игрок
    /// застал в ЭТОЙ сессии, а поля контроллера обнуляются сменой карты.
    private HookResult OnRoundEnd(EventRoundEnd ev, GameEventInfo info)
    {
        foreach (var session in _sessions.Snapshot())
            session.NoteRoundEnd();

        return HookResult.Continue;
    }

    private HookResult OnPlayerTeam(EventPlayerTeam ev, GameEventInfo info)
    {
        var player = ev.Userid;
        if (player is null || player.IsBot) return HookResult.Continue;

        if (_sessions.TryGet(player.SteamID, out var session))
            session.NoteTeam(ev.Team);

        return HookResult.Continue;
    }

    /// Открывает сессию и СРАЗУ пишет строку в базу.
    ///
    /// Писать строку только на выходе нельзя: процесс игрового сервера умирает без
    /// предупреждения, и всё, что жило до этого момента только в памяти, исчезает
    /// вместе с ним. При записи на входе оборванная сессия остаётся видимым фактом
    /// (ended_at IS NULL), а «кто сейчас онлайн» — это обычный SELECT, без RCON и A2S.
    private void OpenSessionFor(CCSPlayerController player)
    {
        ulong steamId;
        string nickname;

        try
        {
            steamId = player.SteamID;
            nickname = Truncate(player.PlayerName, NicknameMaxLength);
        }
        catch (Exception ex)
        {
            _logger.Debug($"[JOIN] контроллер невалиден, сессия не открыта: {ex.Message}");
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

    /// Закрывает сессию. Контроллер может быть уже невалиден — тогда итоги матча
    /// не снимутся, но сама сессия всё равно будет закрыта корректно.
    private void CloseSession(OpenSession session, CCSPlayerController? player, SessionEndKind kind,
        int reason, string reasonName)
    {
        var endedAt = DateTime.UtcNow;
        var duration = (int)Math.Max(0, (endedAt - session.StartedAt).TotalSeconds);

        if (player != null)
        {
            try
            {
                var name = Truncate(player.PlayerName, NicknameMaxLength);
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
            DisconnectMap = CurrentMap(),
            DisconnectReason = reason,
            DisconnectReasonName = Truncate(reasonName, 64),
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
    /// Контроллеры здесь не ищем: на выгрузке плагина и на смене карты они уже
    /// могут быть недействительны, а брать игрока по номеру слота нельзя —
    /// GetPlayerFromSlot не проверяет тип сущности и роняет сервер без стека.
    private void CloseAllSessions(SessionEndKind kind)
    {
        foreach (var session in _sessions.TakeAll())
            CloseSession(session, null, kind, 0, kind.ToString());
    }

    private string? ReadPlayerIp(CCSPlayerController player)
    {
        if (!Config.Collect.PlayerIp && !Config.Collect.IpHash && !Config.Collect.GeoIp) return null;

        try
        {
            // IpAddress бросает InvalidOperationException, если сущность уже невалидна
            return IpUtil.ExtractIp(player.IpAddress);
        }
        catch (Exception ex)
        {
            _logger.Debug($"[JOIN] IP игрока недоступен: {ex.Message}");
            return null;
        }
    }

    private string? ReadClientLanguage(CCSPlayerController player)
    {
        try
        {
            return CounterStrikeSharp.API.Core.Translations.PlayerLanguageExtensions
                .GetLanguage(player)?.TwoLetterISOLanguageName;
        }
        catch (Exception ex)
        {
            _logger.Debug($"[JOIN] язык клиента недоступен: {ex.Message}");
            return null;
        }
    }

    /// Игроков берём ТОЛЬКО через Utilities.GetPlayers(): он фильтрует по IsValid
    /// и Connected, тогда как GetPlayerFromSlot(slot) конструирует контроллер
    /// из произвольной сущности без проверки её типа.
    private static int CountHumans()
    {
        var count = 0;
        foreach (var player in CounterStrikeSharp.API.Utilities.GetPlayers())
        {
            if (!player.IsBot) count++;
        }

        return count;
    }

    private static int SafeMaxPlayers()
    {
        try
        {
            return CounterStrikeSharp.API.Server.MaxPlayers;
        }
        catch (Exception)
        {
            return 0;
        }
    }

    private static string? NullIfEmpty(string? value) => string.IsNullOrEmpty(value) ? null : value;

    /// Ник — недоверенные данные: длину задаёт игрок, а колонка в базе конечна.
    internal static string Truncate(string? value, int maxLength)
    {
        if (string.IsNullOrEmpty(value)) return string.Empty;
        return value.Length <= maxLength ? value : value[..maxLength];
    }

    /// Имя причины отключения из Valve-шного перечисления, без префикса NETWORK_DISCONNECT_.
    /// Неизвестный код не теряем: он остаётся числом в disconnect_reason.
    internal static string ReasonName(int reason)
    {
        var name = Enum.IsDefined(typeof(NetworkDisconnectionReason), reason)
            ? Enum.GetName(typeof(NetworkDisconnectionReason), reason)
            : null;

        if (string.IsNullOrEmpty(name))
            return string.Create(CultureInfo.InvariantCulture, $"REASON_{reason}");

        const string prefix = "NETWORK_DISCONNECT_";
        return name.StartsWith(prefix, StringComparison.Ordinal) ? name[prefix.Length..] : name;
    }
}
