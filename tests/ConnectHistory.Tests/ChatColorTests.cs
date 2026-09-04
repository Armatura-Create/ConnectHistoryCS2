using System;
using CounterStrikeSharp.API.Modules.Utils;
using Xunit;

namespace ConnectHistory.Tests;

/// Движок CS2 съедает цветовой код, стоящий в самом начале сообщения.
/// Регрессия, ради которой этот тест существует: «оптимизация» с ранним выходом,
/// если строка уже начинается с кода цвета, отключает починку ровно в том
/// единственном случае, когда она нужна.
public class ChatColorTests
{
    private static readonly string Green = ChatColors.Green.ToString();
    private static readonly string Default = ChatColors.Default.ToString();

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
    public void IsIdempotent()
    {
        var once = ChatFormat.EnsureChatColorPrefix(Green + "Текст");
        Assert.Equal(once, ChatFormat.EnsureChatColorPrefix(once));
        Assert.Equal(once, ChatFormat.EnsureChatColorPrefix(ChatFormat.EnsureChatColorPrefix(once)));
    }

    [Fact]
    public void ColorInTheMiddleAlsoGetsThePrefix()
    {
        // Дешевле привести к одному виду все окрашенные строки, чем гадать,
        // где в шаблоне окажется первый код после подстановки значений
        var result = ChatFormat.EnsureChatColorPrefix("Привет " + Green + "мир");
        Assert.StartsWith(Default + " ", result, StringComparison.Ordinal);
    }

    [Fact]
    public void PlainTextIsLeftAlone()
    {
        // Иначе к каждой строке без цветов приписывался бы лишний пробел
        Assert.Equal("Просто текст", ChatFormat.EnsureChatColorPrefix("Просто текст"));
        Assert.Equal(string.Empty, ChatFormat.EnsureChatColorPrefix(string.Empty));
    }

    [Fact]
    public void RenderedTemplateFromMessagesJson_SurvivesTheWholePath()
    {
        // Дефолтный prefix начинается с {GREEN} — это и есть тот самый случай
        var messages = ConfigService.CreateDefaultMessages().Messages!;
        var template = messages["playtime"]["RU"];

        var rendered = ChatFormat.Render(template, new System.Collections.Generic.Dictionary<string, string>
        {
            ["{prefix}"] = messages["prefix"]["RU"],
            ["{TOTAL}"] = "3h 10m",
            ["{SESSIONS}"] = "7",
            ["{FIRST}"] = "2026-09-01 12:00"
        });

        var forChat = ChatFormat.EnsureChatColorPrefix(rendered);

        Assert.StartsWith(Default + " " + Green, forChat, StringComparison.Ordinal);
        Assert.DoesNotContain("{", forChat, StringComparison.Ordinal);
    }
}
