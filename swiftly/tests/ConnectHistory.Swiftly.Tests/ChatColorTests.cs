using System;
using SwiftlyS2.Shared;
using Xunit;

namespace ConnectHistory.Tests;

/// Движок CS2 съедает цветовой код, стоящий в самом начале сообщения.
/// Регрессия, ради которой этот тест существует: «оптимизация» с ранним выходом,
/// если строка уже начинается с кода цвета, отключает починку ровно в том
/// единственном случае, когда она нужна.
public class ChatColorTests
{
    private const string Green = "\x04";
    private const string Default = "\x01";

    [Fact]
    public void ColorAtTheVeryStart_GetsAPrefix()
    {
        var result = ChatFormat.EnsureChatColorPrefix(Green + "[История]");

        Assert.StartsWith(Default + " ", result, StringComparison.Ordinal);
        Assert.Contains(Green + "[История]", result, StringComparison.Ordinal);
    }

    [Fact]
    public void LeadingSpaceIsNotDoubled()
    {
        var result = ChatFormat.EnsureChatColorPrefix(" " + Green + "Текст");
        Assert.Equal(Default + " " + Green + "Текст", result);
    }

    [Fact]
    public void Idempotent()
    {
        var once = ChatFormat.EnsureChatColorPrefix(Green + "Текст");
        Assert.Equal(once, ChatFormat.EnsureChatColorPrefix(once));
    }

    [Fact]
    public void PlainTextIsLeftAlone()
    {
        Assert.Equal("просто текст", ChatFormat.EnsureChatColorPrefix("просто текст"));
    }

    /// Коды цветов — это байты движка CS2, а не изобретение фреймворка.
    /// Если таблица в ChatFormat разъедется с таблицей SwiftlyS2, один и тот же
    /// Messages.json начнёт выглядеть по-разному под разными целями.
    [SkippableTheory]
    [InlineData("{DEFAULT}", "[default]")]
    [InlineData("{GREEN}", "[green]")]
    [InlineData("{RED}", "[red]")]
    [InlineData("{OLIVE}", "[olive]")]
    [InlineData("{GREY}", "[grey]")]
    [InlineData("{GOLD}", "[gold]")]
    [InlineData("{PURPLE}", "[purple]")]
    [InlineData("{LIGHTBLUE}", "[lightblue]")]
    public void ColorCodesMatchSwiftlyTable(string ourTag, string swiftlyTag)
    {
        Skip.IfNot(SwiftlyRuntime.Available, SwiftlyRuntime.SkipReason);
        Bound.SameCode(ourTag, swiftlyTag);
    }

    /// Всё, что трогает типы SwiftlyS2 — см. комментарий в SwiftlyRuntime.
    private static class Bound
    {
        public static void SameCode(string ourTag, string swiftlyTag)
            => Assert.Equal(swiftlyTag.Colored(), ChatFormat.Render(ourTag));
    }
}
