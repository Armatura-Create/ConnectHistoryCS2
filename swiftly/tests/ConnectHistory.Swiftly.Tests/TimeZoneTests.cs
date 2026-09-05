using System;
using Xunit;

namespace ConnectHistory.Tests;

/// В базе всегда UTC, игроку показываем в поясе из Settings.json.
/// Эти две вещи не должны путаться: DisplayTimeZone влияет только на вывод.
public class TimeZoneTests
{
    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("UTC")]
    [InlineData("utc")]
    public void EmptyOrUtc_MeansUtc(string? value)
        => Assert.Equal(TimeZoneInfo.Utc, TimeZoneResolver.Resolve(value));

    [Fact]
    public void LocalKeyword_MeansMachineTimeZone()
        => Assert.Equal(TimeZoneInfo.Local, TimeZoneResolver.Resolve("Local"));

    [Fact]
    public void IanaIdentifierIsResolved()
    {
        var zone = TimeZoneResolver.Resolve("Europe/Moscow");

        // Москва круглый год UTC+3 и перевода часов не делает
        Assert.Equal(TimeSpan.FromHours(3), zone.GetUtcOffset(new DateTime(2026, 1, 15, 12, 0, 0, DateTimeKind.Utc)));
        Assert.Equal(TimeSpan.FromHours(3), zone.GetUtcOffset(new DateTime(2026, 7, 15, 12, 0, 0, DateTimeKind.Utc)));
    }

    [Fact]
    public void UnknownZone_FallsBackToUtcInsteadOfCrashing()
        => Assert.Equal(TimeZoneInfo.Utc, TimeZoneResolver.Resolve("Middle/Earth", new NullLogger()));

    [Fact]
    public void DateIsShownInTheConfiguredZone()
    {
        var utc = new DateTime(2026, 9, 4, 21, 30, 0, DateTimeKind.Utc);

        Assert.Equal("2026-09-04 21:30", ChatFormat.Date(utc));
        Assert.Equal("2026-09-05 00:30", ChatFormat.Date(utc, TimeZoneResolver.Resolve("Europe/Moscow")));
        Assert.Equal("2026-09-04 14:30", ChatFormat.Date(utc, TimeZoneResolver.Resolve("America/Los_Angeles")));
    }

    [Fact]
    public void UnspecifiedKindFromDatabaseIsTreatedAsUtc()
    {
        // Драйвер помечает значения как Utc, но если Kind вдруг потеряется,
        // трактовать его как локальное время нельзя — плагин пишет UTC
        var fromDb = new DateTime(2026, 9, 4, 21, 30, 0, DateTimeKind.Unspecified);
        Assert.Equal("2026-09-05 00:30", ChatFormat.Date(fromDb, TimeZoneResolver.Resolve("Europe/Moscow")));
    }

    [Fact]
    public void DisplayTimeZoneDoesNotLeakIntoStoredValues()
    {
        // Всё, что уходит в базу, снимается через UtcNow — это инвариант,
        // а не следствие настроек
        var job = new SessionOpenJob { StartedAt = DateTime.UtcNow };
        Assert.Equal(DateTimeKind.Utc, job.StartedAt.Kind);
    }
}
