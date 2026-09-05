using System;
using System.Collections.Generic;

namespace ConnectHistory;

/// Открытая сессия игрока: всё, что известно на момент входа, плюс то, что копится
/// в течение сессии (пинг, смены команды).
///
/// Живёт в памяти между входом и выходом. Контроллер игрока здесь НЕ хранится: за время
/// сессии объект будет освобождён движком, и обращение к нему — чтение чужой памяти.
public sealed class OpenSession
{
    /// Ключ строки в БД, сгенерированный плагином. INSERT уходит в очередь асинхронно,
    /// поэтому автоинкрементный id на момент выхода игрока может быть ещё не известен.
    public string Key { get; init; } = "";

    public ulong SteamId64 { get; init; }
    public uint AccountId { get; init; }
    public DateTime StartedAt { get; init; }
    public string ConnectMap { get; init; } = "";

    /// Ник обновляется: игрок может сменить его в течение сессии.
    public string Nickname { get; set; } = "";

    public string? CountryIso { get; init; }

    public int PingSamples { get; private set; }
    public int PingMin { get; private set; }
    public int PingMax { get; private set; }
    private long _pingSum;

    public int LastTeam { get; private set; } = -1;
    public int TeamChanges { get; private set; }

    /// Номера команд в CS2: 0 — не выбрана, 1 — наблюдатель, 2 — T, 3 — CT.
    public const int TeamUnassigned = 0;
    public const int TeamSpectator = 1;

    /// Секунды, проведённые в наблюдателях и без команды.
    ///
    /// Копится по событиям смены команды: длительность сессии — это время
    /// подключения, а «наиграно» — время в составе команды. Разделять их
    /// постфактум нельзя, в базе остаётся только итоговая команда.
    public int SpectatorSeconds { get; private set; }

    /// Момент, с которого длится текущее состояние команды.
    private DateTime _teamSince;

    /// Отсчёт времени команды начинается вместе с сессией.
    /// Вызывается фабрикой сессии сразу после создания.
    public void StartTeamTracking(DateTime startedAt) => _teamSince = startedAt;

    /// Сыграно раундов. Считаем событием round_end, а не чтением схемы движка:
    /// поле «сыграно раундов» у контроллера живёт в match stats и обнуляется сменой карты,
    /// а нам нужно ровно то, что игрок застал в ЭТОЙ сессии.
    public int RoundsPlayed { get; private set; }

    /// Среднее по всем снятым замерам. 0, если замеров не было.
    public int PingAvg => PingSamples == 0 ? 0 : (int)(_pingSum / PingSamples);

    /// Нулевой и абсурдный пинг игнорируем: движок отдаёт 0 в первые секунды после входа
    /// и на смене карты, и такие замеры занижали бы среднее.
    public void AddPing(int ping)
    {
        if (ping <= 0 || ping > 2000) return;

        if (PingSamples == 0)
        {
            PingMin = ping;
            PingMax = ping;
        }
        else
        {
            if (ping < PingMin) PingMin = ping;
            if (ping > PingMax) PingMax = ping;
        }

        _pingSum += ping;
        PingSamples++;
    }

    public void NoteRoundEnd() => RoundsPlayed++;

    /// Первая увиденная команда — это не смена, а начальное состояние.
    ///
    /// now передаётся аргументом, а не берётся внутри: так метод остаётся
    /// проверяемым тестом без ожиданий в реальном времени.
    public void NoteTeam(int team, DateTime now)
    {
        if (team == LastTeam) return;

        AccrueTeamTime(now);

        if (LastTeam >= 0) TeamChanges++;
        LastTeam = team;
    }

    /// Закрывает последний интервал команды. Зовётся при закрытии сессии,
    /// иначе время после последней смены команды нигде не учтётся.
    public void FinishTeamTracking(DateTime now) => AccrueTeamTime(now);

    /// Относит истёкший интервал к «вне игры», если игрок провёл его
    /// наблюдателем или без команды.
    ///
    /// Состояние ДО первого события player_team (LastTeam = -1) намеренно
    /// считается игровым: если событие почему-то не придёт вовсе, игрок не
    /// должен остаться с нулевым наигранным временем. Ошибаться безопаснее
    /// в сторону прежнего поведения.
    private void AccrueTeamTime(DateTime now)
    {
        if (_teamSince == default) _teamSince = now;

        if (LastTeam is TeamSpectator or TeamUnassigned)
        {
            var elapsed = (int)Math.Max(0, (now - _teamSince).TotalSeconds);
            SpectatorSeconds += elapsed;
        }

        _teamSince = now;
    }
}

/// Реестр открытых сессий. Обращаются игровые события и колбэк таймера пинга —
/// всё в главном потоке, но lock оставлен сознательно: цена нулевая,
/// а инвариант «словарь не рвётся» держится независимо от будущих изменений.
public sealed class SessionService
{
    private readonly Dictionary<ulong, OpenSession> _sessions = [];
    private readonly object _lock = new();

    public int Count
    {
        get { lock (_lock) return _sessions.Count; }
    }

    public void Add(OpenSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        lock (_lock) _sessions[session.SteamId64] = session;
    }

    public bool TryGet(ulong steamId, out OpenSession session)
    {
        lock (_lock) return _sessions.TryGetValue(steamId, out session!);
    }

    /// Забирает сессию из реестра. Возвращает null, если её там нет:
    /// disconnect может прийти для игрока, которого мы не открывали (бот, ранний выход).
    public OpenSession? Take(ulong steamId)
    {
        lock (_lock)
        {
            if (!_sessions.Remove(steamId, out var session)) return null;
            return session;
        }
    }

    /// Снимок всех открытых сессий — копия, наружу словарь не отдаём.
    public List<OpenSession> TakeAll()
    {
        lock (_lock)
        {
            var all = new List<OpenSession>(_sessions.Values);
            _sessions.Clear();
            return all;
        }
    }

    public List<OpenSession> Snapshot()
    {
        lock (_lock) return [.. _sessions.Values];
    }
}
