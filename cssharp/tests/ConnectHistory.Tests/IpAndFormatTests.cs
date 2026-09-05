using System;
using System.Globalization;
using System.Threading;
using Xunit;

namespace ConnectHistory.Tests;

/// Разбор адреса, обезличивание и форматирование чисел.
public class IpAndFormatTests
{
    [Theory]
    [InlineData("1.2.3.4:27015", "1.2.3.4")]
    [InlineData("1.2.3.4", "1.2.3.4")]
    [InlineData("[2001:db8::1]:27015", "2001:db8::1")]
    [InlineData("2001:db8::1", "2001:db8::1")]     // голый IPv6 — Split(':')[0] его ломал
    [InlineData("", "")]
    [InlineData(null, "")]
    public void ExtractIp_HandlesIpv4AndIpv6(string? input, string expected)
        => Assert.Equal(expected, IpUtil.ExtractIp(input));

    [Theory]
    [InlineData("203.0.113.77", "203.0.113.0/24")]
    [InlineData("2001:db8:1234:5678::1", "2001:db8:1234::/48")]
    [InlineData("not-an-ip", null)]
    public void Subnet_DropsTheHostPart(string input, string? expected)
        => Assert.Equal(expected, IpUtil.ToSubnet(input));

    [Fact]
    public void Hash_IsStableForTheSameSaltAndDiffersAcrossSalts()
    {
        var a = IpUtil.Hash("203.0.113.77", "salt-one");
        var b = IpUtil.Hash("203.0.113.77", "salt-one");
        var c = IpUtil.Hash("203.0.113.77", "salt-two");

        Assert.Equal(a, b);
        Assert.NotEqual(a, c);
        Assert.Equal(64, a!.Length); // CHAR(64) в схеме
    }

    [Fact]
    public void Hash_WithoutSaltIsDisabled()
    {
        // Хеш без соли обратим перебором всего IPv4 за минуты — такая колонка
        // притворялась бы анонимной
        Assert.Null(IpUtil.Hash("203.0.113.77", ""));
        Assert.Null(IpUtil.Hash("203.0.113.77", null));
    }

    [Theory]
    [InlineData("10.1.2.3", true)]
    [InlineData("192.168.0.10", true)]
    [InlineData("172.16.5.5", true)]
    [InlineData("127.0.0.1", true)]
    [InlineData("8.8.8.8", false)]
    [InlineData("garbage", true)]
    public void PrivateRanges_AreRecognized(string ip, bool expected)
        => Assert.Equal(expected, IpUtil.IsLocalOrPrivate(ip));

    [Fact]
    public void Duration_UsesInvariantNumbersOnAnyServerLocale()
    {
        var previous = Thread.CurrentThread.CurrentCulture;
        try
        {
            // Сервер с такой локалью рисовал игрокам другие цифры
            Thread.CurrentThread.CurrentCulture = new CultureInfo("ar-SA");

            Assert.Equal("2h 5m", ChatFormat.Duration(7500));
            Assert.Equal("1d 3h 0m", ChatFormat.Duration(97200));
            Assert.Equal("0s", ChatFormat.Duration(-5));
        }
        finally
        {
            Thread.CurrentThread.CurrentCulture = previous;
        }
    }

    [Fact]
    public void Render_SubstitutesValuesAndStripsKnownTags()
    {
        var rendered = ChatFormat.Render("{GREEN}Привет, {NAME}{DEFAULT}",
            new System.Collections.Generic.Dictionary<string, string> { ["{NAME}"] = "Игрок" });

        Assert.Contains("Игрок", rendered, StringComparison.Ordinal);
        Assert.DoesNotContain("{GREEN}", rendered, StringComparison.Ordinal);
        Assert.DoesNotContain("{DEFAULT}", rendered, StringComparison.Ordinal);
    }

    [Fact]
    public void AccountId_DoesNotOverflowForModernAccounts()
    {
        // account id за 2^31: в signed int такой становится отрицательным.
        // Граница — SteamID64 76561200107749376.
        const ulong steamId = 76561200460265728UL;
        var accountId = SteamIdUtil.ToAccountId(steamId);

        Assert.True(accountId > int.MaxValue, "account_id обязан оставаться беззнаковым");
        Assert.Equal((uint)(steamId - 76561197960265728UL), accountId);
    }

    [Theory]
    [InlineData(0UL, false)]
    [InlineData(76561197960265728UL, false)]
    [InlineData(76561198000000000UL, true)]
    public void BotsAndInvalidControllersAreNotWrittenToHistory(ulong steamId, bool expected)
        => Assert.Equal(expected, SteamIdUtil.IsRealSteamId(steamId));
}
