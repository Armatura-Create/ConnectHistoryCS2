using System;
using Xunit;

namespace ConnectHistory.Tests;

/// Обрезка ника и расшифровка причины отключения.
public class SessionCloseDataTests
{
    [Fact]
    public void Nickname_IsTruncatedToColumnWidth()
    {
        // Ник длиннее колонки роняет весь INSERT с "Data too long",
        // и сессия теряется целиком
        var long_name = new string('я', 500);
        var truncated = PluginText.Truncate(long_name, PluginText.NicknameMaxLength);

        Assert.Equal(PluginText.NicknameMaxLength, truncated.Length);
    }

    [Fact]
    public void ShortNicknameIsUntouched()
        => Assert.Equal("Player", PluginText.Truncate("Player", 128));

    [Fact]
    public void EmptyNicknameBecomesEmptyString()
        => Assert.Equal(string.Empty, PluginText.Truncate(null, 128));

    [SkippableFact]
    public void KnownDisconnectReason_LosesValvePrefix()
    {
        Skip.IfNot(SwiftlyRuntime.Available, SwiftlyRuntime.SkipReason);
        Bound.KnownReasonLosesPrefix();
    }

    [SkippableFact]
    public void UnknownReasonCode_IsKeptAsNumber()
    {
        Skip.IfNot(SwiftlyRuntime.Available, SwiftlyRuntime.SkipReason);
        Bound.UnknownReasonStaysANumber();
    }

    [SkippableFact]
    public void ReasonName_FitsTheColumn()
    {
        Skip.IfNot(SwiftlyRuntime.Available, SwiftlyRuntime.SkipReason);
        Bound.EveryReasonFitsTheColumn();
    }

    /// ReasonName читает перечисление Valve из сборки SwiftlyS2 —
    /// см. комментарий в SwiftlyRuntime.
    private static class Bound
    {
        public static void KnownReasonLosesPrefix()
        {
            // 2 = NETWORK_DISCONNECT_DISCONNECT_BY_USER в перечислении Valve
            var name = ConnectHistory.ReasonName(2);

            Assert.DoesNotContain("NETWORK_DISCONNECT_", name, StringComparison.Ordinal);
            Assert.NotEmpty(name);
        }

        public static void UnknownReasonStaysANumber()
            // Незнакомый код не теряем: он остаётся числом в disconnect_reason
            => Assert.Equal("REASON_31337", ConnectHistory.ReasonName(31337));

        public static void EveryReasonFitsTheColumn()
        {
            for (var reason = 0; reason < 140; reason++)
                Assert.True(ConnectHistory.ReasonName(reason).Length <= 64);
        }
    }
}
