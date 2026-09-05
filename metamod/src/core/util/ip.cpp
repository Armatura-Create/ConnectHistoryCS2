#include "core/util/ip.h"

#include "core/util/sha256.h"

#include <array>
#include <cctype>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace ch {
namespace ip {
namespace {

std::string Trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

bool ParseV4(const std::string& ip, uint8_t out[4]) {
    return inet_pton(AF_INET, ip.c_str(), out) == 1;
}

bool ParseV6(const std::string& ip, uint8_t out[16]) {
    return inet_pton(AF_INET6, ip.c_str(), out) == 1;
}

bool IsIpv6(const std::string& ip) {
    uint8_t bytes[16];
    return ParseV6(ip, bytes);
}

bool IsParsable(const std::string& ip) {
    uint8_t v4[4];
    uint8_t v6[16];
    return ParseV4(ip, v4) || ParseV6(ip, v6);
}

// «Любой интерфейс»: 0.0.0.0 и ::. Записать такое как адрес сервера означает
// записать значение, по которому нельзя подключиться.
bool IsAnyAddress(const std::string& ip) {
    uint8_t v4[4];
    if (ParseV4(ip, v4)) return v4[0] == 0 && v4[1] == 0 && v4[2] == 0 && v4[3] == 0;

    uint8_t v6[16];
    if (ParseV6(ip, v6)) {
        for (int i = 0; i < 16; ++i) {
            if (v6[i] != 0) return false;
        }
        return true;
    }
    return false;
}

bool IsUsablePublicHost(const std::string& ip) {
    if (ip.empty()) return false;
    if (!IsParsable(ip)) return false;
    if (IsAnyAddress(ip)) return false;
    return !IsLocalOrPrivate(ip);
}

std::string FormatAddress(const std::string& host, int port) {
    const std::string wrapped = IsIpv6(host) ? "[" + host + "]" : host;
    return wrapped + ":" + std::to_string(port);
}

// Порт из "host:port" или "[v6]:port". Голый IPv6 порта не содержит.
//
// false — порт указан, но прочитать его нельзя (пустой, с буквами, со знаком).
// true с hasPort == false — порта нет вовсе, и это нормально: возьмётся из ConVar.
// Различать эти два случая обязательно: подставить чужой порт вместо опечатки
// значит получить правдоподобный, но неверный адрес.
bool TryExtractPort(const std::string& value, bool* hasPort, int* port) {
    *hasPort = false;
    *port = 0;

    size_t separator = std::string::npos;

    if (!value.empty() && value[0] == '[') {
        const size_t bracket = value.find("]:");
        if (bracket != std::string::npos && bracket > 0) separator = bracket + 1;
    } else {
        const size_t first = value.find(':');
        const size_t last = value.rfind(':');
        if (first != std::string::npos && first == last) separator = last;
    }

    if (separator == std::string::npos) return true;
    if (separator + 1 >= value.size()) return false;

    const std::string digits = value.substr(separator + 1);

    // Только цифры: "+27015" и " 27015" — тоже опечатки, а не адрес
    for (const char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    if (digits.size() > 5) return false;

    *hasPort = true;
    *port = std::stoi(digits);
    return true;
}

}  // namespace

std::string ExtractIp(const std::string& rawAddress) {
    const std::string value = Trim(rawAddress);
    if (value.empty()) return std::string();

    if (value[0] == '[') {
        const size_t close = value.find(']');
        if (close != std::string::npos && close > 1) return value.substr(1, close - 1);

        std::string stripped = value;
        while (!stripped.empty() && stripped[0] == '[') stripped.erase(0, 1);
        return stripped;
    }

    const size_t lastColon = value.rfind(':');
    if (lastColon == std::string::npos) return value;

    // Несколько двоеточий без скобок — это голый IPv6, порта там нет
    if (value.find(':') != lastColon) return value;

    return value.substr(0, lastColon);
}

bool IsLocalOrPrivate(const std::string& ip) {
    uint8_t v4[4];
    if (ParseV4(ip, v4)) {
        if (v4[0] == 127) return true;                                   // loopback
        if (v4[0] == 10) return true;                                    // 10.0.0.0/8
        if (v4[0] == 172 && v4[1] >= 16 && v4[1] <= 31) return true;     // 172.16.0.0/12
        if (v4[0] == 192 && v4[1] == 168) return true;                   // 192.168.0.0/16
        if (v4[0] == 169 && v4[1] == 254) return true;                   // link-local
        return false;
    }

    uint8_t v6[16];
    if (!ParseV6(ip, v6)) return true;  // не разобрали — считаем непригодным

    // ::1
    bool loopback = v6[15] == 1;
    for (int i = 0; i < 15 && loopback; ++i) {
        if (v6[i] != 0) loopback = false;
    }
    if (loopback) return true;

    if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80) return true;  // fe80::/10 link-local
    if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0xC0) return true;  // fec0::/10 site-local
    return (v6[0] & 0xFE) == 0xFC;                             // fc00::/7 unique-local
}

std::string ToSubnet(const std::string& ip) {
    uint8_t v4[4];
    if (ParseV4(ip, v4)) {
        return std::to_string(v4[0]) + "." + std::to_string(v4[1]) + "." +
               std::to_string(v4[2]) + ".0/24";
    }

    uint8_t v6[16];
    if (!ParseV6(ip, v6)) return std::string();

    for (int i = 6; i < 16; ++i) v6[i] = 0;

    char text[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, v6, text, sizeof(text)) == nullptr) return std::string();

    return std::string(text) + "/48";
}

std::string Hash(const std::string& ip, const std::string& salt) {
    if (ip.empty() || salt.empty()) return std::string();
    return HmacSha256Hex(salt, ip);
}

std::string NormalizeAddress(const std::string& configured, int fallbackPort) {
    const std::string value = Trim(configured);
    if (value.empty()) return std::string();

    const std::string host = ExtractIp(value);
    if (host.empty()) return std::string();

    // Хост может быть и доменом — тогда проверка «не 0.0.0.0» неприменима,
    // но адрес привязки и локальные адреса отсечь всё равно нужно.
    if (IsParsable(host) && !IsUsablePublicHost(host)) return std::string();

    bool hasPort = false;
    int explicitPort = 0;
    if (!TryExtractPort(value, &hasPort, &explicitPort)) return std::string();

    const int port = hasPort ? explicitPort : fallbackPort;
    if (port <= 0 || port > 65535) return std::string();

    return FormatAddress(host, port);
}

std::string ResolvePublicAddress(const std::string& configured,
                                 const std::string& engineIp,
                                 int port) {
    if (!Trim(configured).empty()) return NormalizeAddress(configured, port);

    const std::string ip = ExtractIp(engineIp);
    if (!IsUsablePublicHost(ip)) return std::string();
    if (port <= 0 || port > 65535) return std::string();

    return FormatAddress(ip, port);
}

}  // namespace ip
}  // namespace ch
