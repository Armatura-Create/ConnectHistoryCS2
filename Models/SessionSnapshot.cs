using System;
using System.Text.Json.Serialization;

namespace ConnectHistory;

/// Как закончилась сессия. Пишется в ch_sessions.end_kind.
///
/// Open существует потому, что строка сессии создаётся на ВХОДЕ игрока, а не на выходе:
/// упавший сервер иначе не оставляет о сессии никаких следов вообще.
public enum SessionEndKind
{
    /// Сессия открыта и ещё не закрыта (в БД такого значения нет — там NULL в ended_at)
    Open = 0,

    /// Обычный выход игрока (событие player_disconnect)
    Disconnect = 1,

    /// Смена карты: движок отключает всех, но это не «уход» игрока
    MapChange = 2,

    /// Штатная остановка сервера
    Shutdown = 3,

    /// Выгрузка/перезагрузка плагина
    PluginUnload = 4,

    /// Сессия найдена открытой при старте плагина — сервер до этого умер аварийно
    Stale = 5
}

/// Игровые итоги сессии. Снимаются с контроллера ТОЛЬКО в главном потоке.
public sealed class MatchStatsSnapshot
{
    public int Kills { get; set; }
    public int Deaths { get; set; }
    public int Assists { get; set; }
    public int HeadShots { get; set; }
    public int Damage { get; set; }
    public int Mvps { get; set; }
    public int Score { get; set; }
    public int RoundsPlayed { get; set; }

    /// Финальная команда: 1 spectator, 2 T, 3 CT
    public int Team { get; set; }

    public int TeamChanges { get; set; }
}

/// Единица работы для фонового писателя.
///
/// Полиморфная сериализация нужна не ради красоты: при недоступной БД очередь
/// сбрасывается в JSONL-спул и досылается при следующем старте, а значит тип задания
/// обязан переживать перезапуск процесса.
[JsonPolymorphic(TypeDiscriminatorPropertyName = "$kind")]
[JsonDerivedType(typeof(ServerUpsertJob), "server")]
[JsonDerivedType(typeof(SessionOpenJob), "open")]
[JsonDerivedType(typeof(SessionCloseJob), "close")]
[JsonDerivedType(typeof(OnlineSnapshotJob), "snapshot")]
public abstract class WriteJob
{
    /// Сколько раз задание уже пытались записать. Растёт при ретраях.
    public int Attempts { get; set; }

    /// Короткое описание для логов.
    [JsonIgnore]
    public abstract string Describe { get; }
}

/// Регистрация сервера в справочнике: адрес и hostname меняются, id — нет.
public sealed class ServerUpsertJob : WriteJob
{
    public int ServerId { get; set; }
    public string Address { get; set; } = "";
    public string Hostname { get; set; } = "";
    public DateTime SeenAt { get; set; }

    public override string Describe => $"server #{ServerId} ({Address})";
}

/// Открытие сессии — INSERT на входе игрока.
public sealed class SessionOpenJob : WriteJob
{
    /// Ключ, сгенерированный плагином. Именно по нему потом идёт UPDATE:
    /// автоинкрементный id из БД в момент выхода игрока может быть ещё не известен,
    /// потому что запись идёт асинхронно.
    public string SessionKey { get; set; } = "";

    public ulong SteamId64 { get; set; }
    public uint AccountId { get; set; }
    public int ServerId { get; set; }
    public string Nickname { get; set; } = "";
    public DateTime StartedAt { get; set; }
    public string ConnectMap { get; set; } = "";
    public int PlayersOnline { get; set; }
    public int MaxPlayers { get; set; }
    public string? ClientLang { get; set; }

    public string? PlayerIp { get; set; }
    public string? IpHash { get; set; }
    public string? IpSubnet { get; set; }
    public string? CountryIso { get; set; }
    public string? CountryName { get; set; }
    public string? City { get; set; }

    public string PluginVersion { get; set; } = "";

    public override string Describe => $"open {SteamId64} ({Nickname})";
}

/// Закрытие сессии — UPDATE по SessionKey плюс агрегаты и история ников.
public sealed class SessionCloseJob : WriteJob
{
    public string SessionKey { get; set; } = "";
    public ulong SteamId64 { get; set; }
    public uint AccountId { get; set; }
    public int ServerId { get; set; }

    /// Ник на момент выхода: игрок мог сменить его в течение сессии.
    public string Nickname { get; set; } = "";

    public DateTime StartedAt { get; set; }
    public DateTime EndedAt { get; set; }
    public int DurationSeconds { get; set; }

    /// Сколько из них игрок провёл наблюдателем или без команды.
    public int SpectatorSeconds { get; set; }

    /// Учитывать ли наблюдательское время в «наиграно» (ch_players.total_seconds).
    /// Снимок настройки на момент закрытия: задание может пролежать в спуле,
    /// и настройка за это время способна измениться — но строка обязана попасть
    /// в базу по тем правилам, по которым была собрана.
    public bool CountSpectatorTime { get; set; } = true;

    public string DisconnectMap { get; set; } = "";
    public int DisconnectReason { get; set; }
    public string DisconnectReasonName { get; set; } = "";
    public SessionEndKind EndKind { get; set; } = SessionEndKind.Disconnect;

    public MatchStatsSnapshot? Stats { get; set; }

    public int PingAvg { get; set; }
    public int PingMin { get; set; }
    public int PingMax { get; set; }
    public int PingSamples { get; set; }

    public override string Describe => $"close {SteamId64} ({Nickname}, {DurationSeconds}s)";
}

/// Точка графика посещаемости.
public sealed class OnlineSnapshotJob : WriteJob
{
    public int ServerId { get; set; }
    public DateTime TakenAt { get; set; }
    public int Players { get; set; }
    public int Bots { get; set; }
    public int MaxPlayers { get; set; }
    public string Map { get; set; } = "";

    public override string Describe => $"snapshot server #{ServerId}: {Players} players";
}
