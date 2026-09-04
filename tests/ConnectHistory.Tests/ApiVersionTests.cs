using System.Reflection;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes;
using Xunit;

namespace ConnectHistory.Tests;

/// Регрессия на реальный инцидент из соседнего плагина: MinimumApiVersion поставили
/// «посвежее», чем версия, против которой собирались, и сервер на более старой сборке
/// отказался грузить плагин.
///
/// Правило: собираемся против МИНИМАЛЬНОЙ поддерживаемой версии, и MinimumApiVersion
/// равен ей. Тогда компиляция сама доказывает, что API из новых сборок не используется.
public class ApiVersionTests
{
    [Fact]
    public void MinimumApiVersion_MatchesTheCounterStrikeSharpBuildWeCompileAgainst()
    {
        var declared = typeof(ConnectHistory).GetCustomAttribute<MinimumApiVersion>();
        Assert.NotNull(declared);

        var referencedBuild = typeof(BasePlugin).Assembly.GetName().Version!.Build;
        Assert.Equal(referencedBuild, declared!.Version);
    }

    [Fact]
    public void MinimumApiVersion_IsAtLeastTheDotnet10Boundary()
    {
        // 1.0.369 — первая версия CounterStrikeSharp на .NET 10.
        // Ниже неё net10.0-плагин физически не загрузится.
        var declared = typeof(ConnectHistory).GetCustomAttribute<MinimumApiVersion>();
        Assert.True(declared!.Version >= 369, $"MinimumApiVersion {declared.Version} ниже границы .NET 10 (369)");
    }
}
