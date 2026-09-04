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
