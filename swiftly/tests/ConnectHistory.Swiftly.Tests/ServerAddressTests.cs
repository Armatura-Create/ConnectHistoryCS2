using Xunit;

namespace ConnectHistory.Tests;

/// Публичный адрес сервера.
///
/// Причина существования этих тестов: в базе оказывался "0.0.0.0:27015", и правка
/// строки руками не держалась — плагин перезаписывал её при каждом старте.
/// Корней у проблемы два, и проверяются оба:
///   1. процесс сервера не знает своего публичного адреса (ConVar ip — это адрес
///      привязки сокета), поэтому значение обязано браться из конфига;
///   2. отсутствие адреса не должно затирать уже записанный (SQL в SessionWriter).
public class ServerAddressTests
{
    [Fact]
    public void ConfiguredAddressWinsOverConVar()
    {
        // Человек знает публичный адрес, процесс — нет. Конфиг важнее всегда.
        Assert.Equal(
            "203.0.113.10:27015",
            IpUtil.ResolvePublicAddress("203.0.113.10:27015", "10.0.0.5", 27016));
    }

    [Fact]
    public void ConVarIsUsedOnlyWhenConfigIsEmpty()
    {
        Assert.Equal("203.0.113.10:27015", IpUtil.ResolvePublicAddress("", "203.0.113.10", 27015));
        Assert.Equal("203.0.113.10:27015", IpUtil.ResolvePublicAddress(null, "203.0.113.10", 27015));
    }

    /// Главный случай: сервер слушает все интерфейсы.
    [Theory]
    [InlineData("0.0.0.0")]
    [InlineData("::")]
    public void BindAddressIsRejectedInsteadOfBeingStored(string convarIp)
        => Assert.Equal("", IpUtil.ResolvePublicAddress("", convarIp, 27015));

    [Theory]
    [InlineData("127.0.0.1")]
    [InlineData("10.0.0.5")]
    [InlineData("192.168.1.10")]
    [InlineData("172.16.0.9")]
    [InlineData("169.254.1.1")]
    public void LocalAndPrivateAddressesAreRejected(string convarIp)
        => Assert.Equal("", IpUtil.ResolvePublicAddress("", convarIp, 27015));

    /// Явно вписанный в конфиг 0.0.0.0 — тоже не адрес. Уважать такую настройку
    /// значило бы вернуть ровно ту проблему, ради которой настройка добавлена.
    [Theory]
    [InlineData("0.0.0.0:27015")]
    [InlineData("0.0.0.0")]
    [InlineData("127.0.0.1:27015")]
    [InlineData("192.168.0.10:27015")]
    public void UselessConfiguredAddressIsRejected(string configured)
        => Assert.Equal("", IpUtil.ResolvePublicAddress(configured, "203.0.113.10", 0));

    /// Заполненная, но непригодная настройка НЕ подменяется автоопределением,
    /// даже когда ConVar в кои-то веки отдаёт годный адрес: иначе в базе окажется
    /// адрес, которого человек не писал, а его опечатка так и останется незамеченной.
    [Theory]
    [InlineData(":")]
    [InlineData(":27015")]
    [InlineData("203.0.113.10:0")]
    [InlineData("203.0.113.10:70000")]
    public void BrokenConfigIsNotSilentlyReplacedByAutoDetection(string configured)
        => Assert.Equal("", IpUtil.ResolvePublicAddress(configured, "203.0.113.10", 27015));

    /// Пробелы — это «не заполнено», а не «заполнено неверно»: автоопределение работает.
    [Fact]
    public void BlankConfigStillFallsBackToConVar()
        => Assert.Equal("203.0.113.10:27015", IpUtil.ResolvePublicAddress("   ", "203.0.113.10", 27015));

    /// Опечатка в порту — не то же самое, что отсутствие порта.
    /// Подставить сюда порт из ConVar значило бы вернуть правдоподобный,
    /// но неверный адрес — и человек никогда не узнает, что ошибся.
    [Theory]
    [InlineData("203.0.113.10:порт")]
    [InlineData("203.0.113.10:")]
    [InlineData("203.0.113.10:+27015")]
    [InlineData("203.0.113.10: 27015")]
    public void UnreadablePortIsAnErrorNotAnOmission(string configured)
        => Assert.Equal("", IpUtil.ResolvePublicAddress(configured, "203.0.113.10", 27015));

    /// Порт можно не писать: он берётся из ConVar hostport.
    [Fact]
    public void PortIsTakenFromConVarWhenConfigOmitsIt()
        => Assert.Equal("203.0.113.10:27015", IpUtil.ResolvePublicAddress("203.0.113.10", "0.0.0.0", 27015));

    /// Домен — законный публичный адрес: проверку «не приватный» к нему не применить,
    /// но и отбрасывать его нельзя.
    [Fact]
    public void HostnameIsAccepted()
        => Assert.Equal("cs2.example.com:27015", IpUtil.ResolvePublicAddress("cs2.example.com:27015", "", 0));

    /// IPv6 записывается в скобках, иначе двоеточие порта неотличимо от адреса.
    [Theory]
    [InlineData("[2001:db8::1]:27015", "[2001:db8::1]:27015")]
    [InlineData("2001:db8::1", "[2001:db8::1]:27015")]
    public void Ipv6KeepsBrackets(string configured, string expected)
        => Assert.Equal(expected, IpUtil.ResolvePublicAddress(configured, "", 27015));

    [Fact]
    public void ZeroPortWithoutConfigProducesNothing()
    {
        // ConVar hostport ещё не поднят — лучше пусто, чем "203.0.113.10:0"
        Assert.Equal("", IpUtil.ResolvePublicAddress("", "203.0.113.10", 0));
    }

    /// Пустой результат — законный ответ «адрес неизвестен», и SQL обязан его
    /// игнорировать, а не затирать им ранее записанный адрес. Иначе исправленная
    /// строка снова портится на следующем рестарте.
    [Fact]
    public void EmptyAddressDoesNotOverwriteStoredOne()
    {
        var sql = SessionWriter.ServerUpsertSql("ch_");

        Assert.Contains("IF(VALUES(`address`) = '', `address`, VALUES(`address`))", sql);
        Assert.Contains("IF(VALUES(`hostname`) = '', `hostname`, VALUES(`hostname`))", sql);
        Assert.Contains("`ch_servers`", sql);
    }
}
