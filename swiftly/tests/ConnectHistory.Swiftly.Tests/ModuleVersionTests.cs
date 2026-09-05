using System;
using System.Reflection;
using SwiftlyS2.Shared;
using Xunit;

namespace ConnectHistory.Tests;

/// Версия не хардкодится: её источник — <Version> в .csproj (и -p:Version= в CI).
/// Иначе номер приходится помнить поднять в двух местах, и релиз уезжает со старым.
public class ModuleVersionTests
{
    [Fact]
    public void InformationalVersion_LosesSourceLinkSuffix()
        => Assert.Equal("v2.0.0", PluginText.FormatModuleVersion("2.0.0+abc123", new Version(2, 0, 0)));

    [Fact]
    public void PreReleaseSuffix_IsKept()
        => Assert.Equal("v2.1.0-rc1", PluginText.FormatModuleVersion("2.1.0-rc1", new Version(2, 1, 0)));

    [Fact]
    public void FallsBackToAssemblyVersion()
        => Assert.Equal("v1.2.3", PluginText.FormatModuleVersion(null, new Version(1, 2, 3, 4)));

    [Fact]
    public void WithoutAnythingReturnsZero()
        => Assert.Equal("v0.0.0", PluginText.FormatModuleVersion("   ", null));

    [Fact]
    public void ResolvedVersion_IsNotAHardcodedLiteral()
    {
        var resolved = PluginText.ResolveModuleVersion(typeof(PluginText).Assembly);
        Assert.StartsWith("v", resolved, StringComparison.Ordinal);
        Assert.NotEqual("v0.0.0", resolved);
    }

    /// PluginMetadata.Version — константа времени компиляции, поэтому её генерирует
    /// MSBuild из того же <Version>. Тест ловит возврат к литералу: там всегда
    /// окажется версия, забытая при прошлом релизе.
    [SkippableFact]
    public void PluginMetadataVersion_ComesFromTheAssemblyVersion()
    {
        Skip.IfNot(SwiftlyRuntime.Available, SwiftlyRuntime.SkipReason);
        Bound.MetadataMatchesAssembly();
    }

    /// Всё, что трогает типы SwiftlyS2 — см. комментарий в SwiftlyRuntime.
    private static class Bound
    {
        public static void MetadataMatchesAssembly()
        {
            var declared = typeof(ConnectHistory).GetCustomAttribute<PluginMetadata>()!;
            var resolved = PluginText.ResolveModuleVersion(typeof(PluginText).Assembly);

            Assert.Equal(resolved, "v" + declared.Version);
        }
    }
}
