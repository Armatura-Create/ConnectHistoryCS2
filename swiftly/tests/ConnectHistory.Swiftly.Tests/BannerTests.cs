using System;
using System.Collections.Generic;
using Xunit;

namespace ConnectHistory.Tests;

/// Рамка заставки считается, а не прибита пробелами: версия приходит из тега
/// и может быть длиннее ожидаемого. Разъехавшаяся рамка в консоли сервера —
/// первое, что владелец увидит о плагине.
public class BannerTests
{
    [Theory]
    [InlineData("v1.0.0")]
    [InlineData("v3.0.0-rc1+build.12345")]
    [InlineData("v0.0.0")]
    public void BannerIsARectangleWhateverTheVersionLength(string version)
    {
        IReadOnlyList<string> lines = Banner.Build(version, "CounterStrikeSharp", 1);

        Assert.Equal(7, lines.Count);

        var width = lines[0].Length;
        foreach (var line in lines) Assert.Equal(width, line.Length);

        Assert.StartsWith("+", lines[0], StringComparison.Ordinal);
        Assert.StartsWith("+", lines[^1], StringComparison.Ordinal);
    }

    [Fact]
    public void BannerCarriesVersionPlatformAndServerNumber()
    {
        var all = string.Join("", Banner.Build("v3.0.0", "SwiftlyS2", 42));

        Assert.Contains("ConnectHistory v3.0.0", all, StringComparison.Ordinal);
        Assert.Contains("SwiftlyS2", all, StringComparison.Ordinal);
        Assert.Contains("#42", all, StringComparison.Ordinal);
    }
}
