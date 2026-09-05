using System;
using Xunit;

namespace ConnectHistory.Tests;

/// Арифметика сессии — самое хрупкое место в плагине: состояние обязано ключеваться
/// по SteamID (слоты движок переиспользует), а нулевая метка времени входа
/// превращает session_time в 1.7 млрд секунд.
public class SessionMathTests
{
    private static OpenSession NewSession() => new()
    {
        Key = Guid.NewGuid().ToString("N"),
        SteamId64 = 76561198000000000UL,
        StartedAt = DateTime.UtcNow
    };

    [Fact]
    public void PingAverage_IgnoresZeroSamples()
    {
        var session = NewSession();
        session.AddPing(0);      // движок отдаёт 0 в первые секунды после входа
        session.AddPing(40);
        session.AddPing(60);

        Assert.Equal(2, session.PingSamples);
        Assert.Equal(50, session.PingAvg);
        Assert.Equal(40, session.PingMin);
        Assert.Equal(60, session.PingMax);
    }

    [Fact]
    public void PingAverage_IgnoresAbsurdValues()
    {
        var session = NewSession();
        session.AddPing(50);
        session.AddPing(99999);

        Assert.Equal(1, session.PingSamples);
        Assert.Equal(50, session.PingMax);
    }

    [Fact]
    public void WithoutSamples_AverageIsZeroAndNotDivideByZero()
        => Assert.Equal(0, NewSession().PingAvg);

    [Fact]
    public void FirstTeamIsNotACounterAsAChange()
    {
        var session = NewSession();
        var t0 = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);

        session.NoteTeam(2, t0);
        Assert.Equal(0, session.TeamChanges);

        session.NoteTeam(2, t0.AddMinutes(1)); // то же значение — не смена
        Assert.Equal(0, session.TeamChanges);

        session.NoteTeam(3, t0.AddMinutes(2));
        Assert.Equal(1, session.TeamChanges);
        Assert.Equal(3, session.LastTeam);
    }

    /// Время наблюдателя копится по сменам команды: длительность сессии — это
    /// время подключения, а «наиграно» — время в составе команды. Разделить их
    /// постфактум нельзя, в базе остаётся только итоговая команда.
    [Fact]
    public void SpectatorTimeAccumulatesBetweenTeamChanges()
    {
        var session = NewSession();
        var t0 = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);
        session.StartTeamTracking(t0);

        session.NoteTeam(OpenSession.TeamSpectator, t0);            // ушёл в спектаторы
        session.NoteTeam(3, t0.AddMinutes(10));                     // через 10 минут в CT
        session.NoteTeam(OpenSession.TeamSpectator, t0.AddMinutes(40));
        session.FinishTeamTracking(t0.AddMinutes(45));              // вышел из спектаторов

        Assert.Equal(15 * 60, session.SpectatorSeconds);
    }

    /// Незакрытый последний интервал — самый вероятный способ потерять учёт:
    /// игрок ушёл в спектаторы и просто отключился, смены команды больше не было.
    [Fact]
    public void FinalIntervalIsCountedOnClose()
    {
        var session = NewSession();
        var t0 = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);
        session.StartTeamTracking(t0);

        session.NoteTeam(OpenSession.TeamSpectator, t0.AddMinutes(5));

        Assert.Equal(0, session.SpectatorSeconds);   // интервал ещё идёт

        session.FinishTeamTracking(t0.AddMinutes(25));

        Assert.Equal(20 * 60, session.SpectatorSeconds);
    }

    /// Состояние ДО первого player_team считается игровым сознательно: если
    /// событие не придёт вовсе, игрок не должен остаться с нулевым наигранным.
    [Fact]
    public void TimeBeforeFirstTeamEventIsNotCountedAsSpectator()
    {
        var session = NewSession();
        var t0 = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);
        session.StartTeamTracking(t0);

        session.FinishTeamTracking(t0.AddHours(3));

        Assert.Equal(0, session.SpectatorSeconds);
    }

    /// Без команды (0) — тоже не игра: это время выбора команды после входа.
    [Fact]
    public void UnassignedCountsAsOutOfGame()
    {
        var session = NewSession();
        var t0 = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);
        session.StartTeamTracking(t0);

        session.NoteTeam(OpenSession.TeamUnassigned, t0);
        session.NoteTeam(2, t0.AddMinutes(3));
        session.FinishTeamTracking(t0.AddMinutes(60));

        Assert.Equal(3 * 60, session.SpectatorSeconds);
    }

    [Fact]
    public void PlayingAllSessionLeavesNoSpectatorTime()
    {
        var session = NewSession();
        var t0 = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);
        session.StartTeamTracking(t0);

        session.NoteTeam(2, t0);
        session.NoteTeam(3, t0.AddMinutes(30));
        session.FinishTeamTracking(t0.AddMinutes(60));

        Assert.Equal(0, session.SpectatorSeconds);
    }

    [Fact]
    public void RoundsAreCountedPerSession()
    {
        var session = NewSession();
        session.NoteRoundEnd();
        session.NoteRoundEnd();
        Assert.Equal(2, session.RoundsPlayed);
    }

    [Fact]
    public void TakeRemovesSessionSoDisconnectCannotCloseItTwice()
    {
        var service = new SessionService();
        var session = NewSession();
        service.Add(session);

        Assert.NotNull(service.Take(session.SteamId64));
        Assert.Null(service.Take(session.SteamId64));
        Assert.Equal(0, service.Count);
    }

    [Fact]
    public void SlotReuseCannotLeakAnotherPlayersStartTime()
    {
        // Ключ — SteamID, а не номер слота: именно переиспользование слота
        // отдавало новому игроку время входа предыдущего.
        var service = new SessionService();
        var first = NewSession();
        var second = new OpenSession { Key = "b", SteamId64 = 76561198000000999UL, StartedAt = DateTime.UtcNow };

        service.Add(first);
        service.Add(second);

        Assert.True(service.TryGet(second.SteamId64, out var found));
        Assert.Equal(second.Key, found.Key);
    }

    [Fact]
    public void TakeAllEmptiesRegistry_SoUnloadClosesEverythingOnce()
    {
        var service = new SessionService();
        service.Add(NewSession());
        service.Add(new OpenSession { Key = "b", SteamId64 = 2, StartedAt = DateTime.UtcNow });

        Assert.Equal(2, service.TakeAll().Count);
        Assert.Empty(service.TakeAll());
    }
}
