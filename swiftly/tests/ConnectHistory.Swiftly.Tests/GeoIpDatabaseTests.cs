using System;
using System.IO;
using Xunit;

namespace ConnectHistory.Tests;

/// Гео-база — не абстракция, а файл на диске, который легко положить битым или недокачанным:
/// выглядит он при этом как обычный .mmdb, и узнаёшь об этом только на боевом сервере.
/// Поэтому тест открывает РЕАЛЬНУЮ базу из GeoIP/ и делает по ней запрос.
public class GeoIpDatabaseTests
{
    /// Ищем каталог GeoIP/ вверх от папки сборки: тесты запускают и из корня, и из IDE.
    private static string? FindGeoIpDirectory()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);

        while (directory != null)
        {
            var candidate = Path.Combine(directory.FullName, "GeoIP");
            if (File.Exists(Path.Combine(candidate, "GeoLite2-Country.mmdb"))) return candidate;
            directory = directory.Parent;
        }

        return null;
    }

    [SkippableFact]
    public void CommittedDatabasesResolveAKnownAddress()
    {
        var geoIpDirectory = FindGeoIpDirectory();
        Skip.If(geoIpDirectory == null, "Каталог GeoIP/ с базами не найден");

        using var service = new GeoIpService(geoIpDirectory!, new NullLogger());

        // 8.8.8.8 — Google DNS, США. Если база битая или открылась не тем режимом,
        // это выяснится здесь, а не на игровом сервере.
        var info = service.Lookup("8.8.8.8");

        Assert.Equal("US", info.Iso);
        Assert.False(string.IsNullOrEmpty(info.Country));
    }

    [SkippableFact]
    public void PrivateAddressesAreNotLookedUp()
    {
        var geoIpDirectory = FindGeoIpDirectory();
        Skip.If(geoIpDirectory == null, "Каталог GeoIP/ с базами не найден");

        using var service = new GeoIpService(geoIpDirectory!, new NullLogger());

        // Локальная сеть страны не имеет — и не должна попадать в БД как «страна»
        Assert.Null(service.Lookup("192.168.1.10").Iso);
        Assert.Null(service.Lookup("127.0.0.1").Iso);
        Assert.Null(service.Lookup("не адрес").Iso);
    }

    [Fact]
    public void MissingDatabaseIsNotAnError()
    {
        // Гео отключено или .mmdb не положили рядом с DLL — плагин обязан продолжать
        // писать сессии, просто без страны и города
        using var service = new GeoIpService(Path.GetTempPath(), new NullLogger());
        var info = service.Lookup("8.8.8.8");

        Assert.Null(info.Iso);
        Assert.Null(info.City);
    }
}
