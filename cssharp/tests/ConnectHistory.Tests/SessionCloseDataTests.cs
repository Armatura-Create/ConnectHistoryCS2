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
        var truncated = ConnectHistory.Truncate(long_name, ConnectHistory.NicknameMaxLength);

        Assert.Equal(ConnectHistory.NicknameMaxLength, truncated.Length);
    }

    [Fact]
    public void ShortNicknameIsUntouched()
        => Assert.Equal("Player", ConnectHistory.Truncate("Player", 128));

    [Fact]
    public void EmptyNicknameBecomesEmptyString()
        => Assert.Equal(string.Empty, ConnectHistory.Truncate(null, 128));

    [Fact]
    public void KnownDisconnectReason_LosesValvePrefix()
    {
        // 2 = NETWORK_DISCONNECT_DISCONNECT_BY_USER в перечислении Valve
        var name = ConnectHistory.ReasonName(2);

        Assert.DoesNotContain("NETWORK_DISCONNECT_", name, StringComparison.Ordinal);
        Assert.NotEmpty(name);
    }

    [Fact]
    public void UnknownReasonCode_IsKeptAsNumber()
    {
        // Незнакомый код не теряем: он остаётся числом в disconnect_reason
        Assert.Equal("REASON_31337", ConnectHistory.ReasonName(31337));
    }

    [Fact]
    public void ReasonName_FitsTheColumn()
    {
        for (var reason = 0; reason < 140; reason++)
            Assert.True(ConnectHistory.ReasonName(reason).Length <= 64);
    }
}
