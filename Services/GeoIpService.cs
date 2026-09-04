using System;
using System.IO;
using System.Net;
using MaxMind.Db;
using MaxMind.GeoIP2;

namespace ConnectHistory;

/// Страна и город по IP. Базы GeoLite2 лежат рядом с DLL плагина.
public sealed class GeoIpService : IDisposable
{
    private readonly ILogger _logger;
    private readonly string _moduleDirectory;
    private readonly object _lock = new();

    private DatabaseReader? _countryReader;
    private DatabaseReader? _cityReader;
    private bool _countryTried;
    private bool _cityTried;

    public GeoIpService(string moduleDirectory, ILogger logger)
    {
        _moduleDirectory = moduleDirectory;
        _logger = logger;
    }

    /// Результат поиска. Всё nullable: отсутствие базы, приватный адрес и незнакомый
    /// диапазон — нормальные ситуации, а не ошибки.
    public readonly record struct GeoInfo(string? Iso, string? Country, string? City);

    public GeoInfo Lookup(string? ip)
    {
        if (string.IsNullOrWhiteSpace(ip) || IpUtil.IsLocalOrPrivate(ip)) return default;
        if (!IPAddress.TryParse(ip, out var address)) return default;

        string? iso = null, country = null, city = null;

        try
        {
            var countryReader = EnsureReader(ref _countryReader, ref _countryTried, "GeoLite2-Country.mmdb");
            if (countryReader != null)
            {
                var response = countryReader.Country(address);
                iso = response.Country.IsoCode;
                country = response.Country.Name;
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            _logger.Debug($"[GEO] страна не определена для {ip}: {ex.Message}");
        }

        try
        {
            var cityReader = EnsureReader(ref _cityReader, ref _cityTried, "GeoLite2-City.mmdb");
            if (cityReader != null)
            {
                var response = cityReader.City(address);
                city = response.City?.Name;
                iso ??= response.Country.IsoCode;
                country ??= response.Country.Name;
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            _logger.Debug($"[GEO] город не определён для {ip}: {ex.Message}");
        }

        return new GeoInfo(iso, country, city);
    }

    private DatabaseReader? EnsureReader(ref DatabaseReader? reader, ref bool tried, string fileName)
    {
        if (reader != null) return reader;

        lock (_lock)
        {
            if (reader != null) return reader;
            if (tried) return null;

            tried = true;
            reader = OpenDatabase(fileName);
            return reader;
        }
    }

    /// Открывает базу MaxMind, печатая путь и РАЗМЕР файла.
    ///
    /// Размер в логе не для красоты: базы качаются на этапе сборки, и недокачанный файл
    /// выглядит как обычный — до первого чтения. Ориентиры: Country ~9 МБ, City ~60 МБ.
    private DatabaseReader? OpenDatabase(string fileName)
    {
        var path = Path.Combine(_moduleDirectory, fileName);

        if (!File.Exists(path))
        {
            _logger.Warn($"[GEO] база {fileName} не найдена ({path}) — страна и город собираться не будут");
            return null;
        }

        try
        {
            var size = new FileInfo(path).Length;
            _logger.Debug($"[GEO] открываю {fileName}, размер {size} байт");

            // FileAccessMode.Memory, а НЕ дефолтный MemoryMapped.
            //
            // По умолчанию MaxMind.Db отображает .mmdb в память и читает её страничными
            // отказами. Внутри игрового процесса это фатально: том Docker/overlayfs или
            // движок со своими обработчиками сигналов превращают страничный отказ в SIGBUS,
            // а он убивает процесс мгновенно — без .NET-исключения, без стека, без строки
            // в логе. Реальный инцидент в NotifyMessages при полностью целом файле.
            // Цена режима Memory — RAM размером с базу; ошибка чтения становится обычным
            // исключением.
            var reader = new DatabaseReader(path, FileAccessMode.Memory);
            _logger.Info($"[GEO] {fileName} загружена в память ({size} байт)");
            return reader;
        }
        catch (Exception ex)
        {
            _logger.Error($"[GEO] не удалось открыть {fileName}", ex);
            return null;
        }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            _countryReader?.Dispose();
            _cityReader?.Dispose();
            _countryReader = null;
            _cityReader = null;
        }
    }
}
