using System;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace ConnectHistory;

/// Работа с адресом игрока: разбор, обезличивание, хеширование.
public static class IpUtil
{
    /// Достаёт IP из строки вида "1.2.3.4:27015", "1.2.3.4" или "[::1]:27015".
    /// Наивный Split(':')[0] ломает любой IPv6-адрес.
    public static string ExtractIp(string? rawAddress)
    {
        if (string.IsNullOrWhiteSpace(rawAddress)) return string.Empty;

        var value = rawAddress.Trim();

        if (value.StartsWith('['))
        {
            var close = value.IndexOf(']', StringComparison.Ordinal);
            return close > 1 ? value.Substring(1, close - 1) : value.TrimStart('[');
        }

        var lastColon = value.LastIndexOf(':');
        if (lastColon < 0) return value;

        // Несколько двоеточий без скобок — это голый IPv6, порта там нет
        if (value.IndexOf(':', StringComparison.Ordinal) != lastColon) return value;

        return value[..lastColon];
    }

    /// Публичный адрес сервера для справочника ch_servers.
    ///
    /// Порядок и его причина: значение из конфига важнее любого автоопределения,
    /// потому что публичный адрес — внешний факт. Процесс игрового сервера его
    /// не знает: ConVar ip отдаёт адрес ПРИВЯЗКИ сокета, и при обычной настройке
    /// это 0.0.0.0 («слушаю все интерфейсы»). Записать такое в базу — значит
    /// записать значение, по которому нельзя подключиться.
    ///
    /// Пустая строка на выходе — законный результат, означающий «адрес неизвестен».
    /// Он не затирает уже записанный адрес: см. SessionWriter.WriteServerAsync.
    public static string ResolvePublicAddress(string? configured, string? convarIp, int port)
    {
        // Настройка заполнена — работаем только по ней. Если значение непригодно,
        // это ошибка настройки, и подменять её автоопределением нельзя: человек
        // никогда не узнает об опечатке, а в базе окажется адрес, которого он не писал.
        if (!string.IsNullOrWhiteSpace(configured))
        {
            return NormalizeAddress(configured, port) ?? string.Empty;
        }

        var ip = ExtractIp(convarIp);
        if (!IsUsablePublicHost(ip)) return string.Empty;
        if (port is <= 0 or > 65535) return string.Empty;

        return FormatAddress(ip, port);
    }

    /// Приводит настройку вида "1.2.3.4:27015", "host:27015", "[::1]:27015" или "1.2.3.4"
    /// к каноническому "host:port". Порт можно не указывать — возьмётся из ConVar hostport.
    /// null означает «настройка непригодна», в том числе если человек вписал 0.0.0.0.
    internal static string? NormalizeAddress(string? configured, int fallbackPort)
    {
        if (string.IsNullOrWhiteSpace(configured)) return null;

        var value = configured.Trim();
        var host = ExtractIp(value);

        if (string.IsNullOrWhiteSpace(host)) return null;

        // Хост может быть и доменом — тогда проверка «не 0.0.0.0» неприменима,
        // но пустышки и адрес привязки отсечь всё равно нужно.
        if (IPAddress.TryParse(host, out _) && !IsUsablePublicHost(host)) return null;

        // Отсутствие порта и НЕЧИТАЕМЫЙ порт — разные вещи. Первое означает
        // «возьми из ConVar», второе — опечатку. Подставлять чужой порт вместо
        // опечатки нельзя: получится правдоподобный, но неверный адрес.
        if (!TryExtractPort(value, out var explicitPort)) return null;

        var port = explicitPort ?? fallbackPort;
        if (port is <= 0 or > 65535) return null;

        return FormatAddress(host, port);
    }

    /// Годится ли адрес как публичный: не пустой, разбирается, не «любой интерфейс»,
    /// не loopback и не приватная сеть.
    private static bool IsUsablePublicHost(string? ip)
    {
        if (string.IsNullOrWhiteSpace(ip)) return false;
        if (!IPAddress.TryParse(ip, out var parsed)) return false;
        if (parsed.Equals(IPAddress.Any) || parsed.Equals(IPAddress.IPv6Any)) return false;

        return !IsLocalOrPrivate(ip);
    }

    /// Порт из "host:port" или "[v6]:port". Голый IPv6 порта не содержит.
    ///
    /// false — порт указан, но прочитать его нельзя (пустой, с буквами, со знаком).
    /// true с null в port — порта нет вовсе, и это нормально.
    private static bool TryExtractPort(string value, out int? port)
    {
        port = null;

        var separator = value.StartsWith('[')
            ? value.IndexOf("]:", StringComparison.Ordinal) is var bracket && bracket > 0 ? bracket + 1 : -1
            : value.IndexOf(':', StringComparison.Ordinal) == value.LastIndexOf(':') ? value.LastIndexOf(':') : -1;

        if (separator < 0) return true;
        if (separator + 1 >= value.Length) return false;

        // NumberStyles.None отсекает знак и пробелы: "+27015" и " 27015" — тоже опечатки
        if (!int.TryParse(value[(separator + 1)..], NumberStyles.None, CultureInfo.InvariantCulture, out var parsed))
        {
            return false;
        }

        port = parsed;
        return true;
    }

    /// IPv6 в адресе пишется в скобках, иначе двоеточие порта неотличимо от адреса.
    private static string FormatAddress(string host, int port)
    {
        var isIpv6 = IPAddress.TryParse(host, out var parsed)
                     && parsed.AddressFamily == AddressFamily.InterNetworkV6;

        return string.Create(CultureInfo.InvariantCulture, $"{(isIpv6 ? "[" + host + "]" : host)}:{port}");
    }

    /// Подсеть: /24 для IPv4, /48 для IPv6.
    ///
    /// Нужна для аналитики «тот же провайдер / тот же дом» там, где хранить полный адрес
    /// не хочется: подсеть — это уже не идентификатор конкретного человека.
    public static string? ToSubnet(string? ip)
    {
        if (!IPAddress.TryParse(ip, out var address)) return null;

        var bytes = address.GetAddressBytes();

        if (address.AddressFamily == AddressFamily.InterNetwork)
            return string.Create(CultureInfo.InvariantCulture, $"{bytes[0]}.{bytes[1]}.{bytes[2]}.0/24");

        if (address.AddressFamily != AddressFamily.InterNetworkV6) return null;

        for (var i = 6; i < bytes.Length; i++) bytes[i] = 0;
        return new IPAddress(bytes).ToString() + "/48";
    }

    /// HMAC-SHA256 от адреса.
    ///
    /// Голый SHA256 от IPv4 бесполезен: всё пространство адресов перебирается за минуты,
    /// то есть хеш обратим. Секретная соль делает перебор невозможным без неё.
    /// Пустая соль = хеширование выключено (пусть лучше не будет колонки, чем будет
    /// колонка, притворяющаяся анонимной).
    public static string? Hash(string? ip, string? salt)
    {
        if (string.IsNullOrWhiteSpace(ip) || string.IsNullOrEmpty(salt)) return null;

        var key = Encoding.UTF8.GetBytes(salt);
        var data = Encoding.UTF8.GetBytes(ip);
        return Convert.ToHexStringLower(HMACSHA256.HashData(key, data));
    }

    /// Локальные и приватные адреса гео не имеют — их не за чем и хранить как «страну».
    public static bool IsLocalOrPrivate(string? ip)
    {
        if (!IPAddress.TryParse(ip, out var addr)) return true;
        if (IPAddress.IsLoopback(addr)) return true;

        var bytes = addr.GetAddressBytes();

        if (addr.AddressFamily == AddressFamily.InterNetwork)
        {
            if (bytes[0] == 10) return true;                                  // 10.0.0.0/8
            if (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) return true; // 172.16.0.0/12
            if (bytes[0] == 192 && bytes[1] == 168) return true;              // 192.168.0.0/16
            if (bytes[0] == 169 && bytes[1] == 254) return true;              // link-local
            return false;
        }

        if (addr.AddressFamily != AddressFamily.InterNetworkV6) return true;
        if (addr.IsIPv6LinkLocal || addr.IsIPv6SiteLocal) return true;
        return (bytes[0] & 0xFE) == 0xFC;                                     // fc00::/7
    }
}
