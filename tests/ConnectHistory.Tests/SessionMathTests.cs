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
        session.NoteTeam(2);
        Assert.Equal(0, session.TeamChanges);

        session.NoteTeam(2); // то же значение — не смена
        Assert.Equal(0, session.TeamChanges);

        session.NoteTeam(3);
        Assert.Equal(1, session.TeamChanges);
        Assert.Equal(3, session.LastTeam);
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
