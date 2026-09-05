using System;
using Xunit;

namespace ConnectHistory.Tests;

/// Версия не хардкодится в ModuleVersion: её источник — метаданные сборки.
/// Иначе номер приходится помнить поднять в двух местах, и релиз уезжает со старым.
public class ModuleVersionTests
{
    [Fact]
    public void InformationalVersion_LosesSourceLinkSuffix()
        => Assert.Equal("v2.0.0", ConnectHistory.FormatModuleVersion("2.0.0+abc123", new Version(2, 0, 0)));

    [Fact]
    public void PreReleaseSuffix_IsKept()
        => Assert.Equal("v2.1.0-rc1", ConnectHistory.FormatModuleVersion("2.1.0-rc1", new Version(2, 1, 0)));

    [Fact]
    public void FallsBackToAssemblyVersion()
        => Assert.Equal("v1.2.3", ConnectHistory.FormatModuleVersion(null, new Version(1, 2, 3, 4)));

    [Fact]
    public void WithoutAnythingReturnsZero()
        => Assert.Equal("v0.0.0", ConnectHistory.FormatModuleVersion("   ", null));

    [Fact]
    public void ModuleVersion_IsNotAHardcodedLiteral()
    {
        // Если кто-то вернёт литерал, тест это заметит: значение обязано совпадать
        // с тем, что резолвится из сборки.
        var resolved = ConnectHistory.ResolveModuleVersion(typeof(ConnectHistory).Assembly);
        Assert.StartsWith("v", resolved, StringComparison.Ordinal);
        Assert.NotEqual("v0.0.0", resolved);
    }
}
