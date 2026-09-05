#include "core/util/timeutil.h"

#include "core/logger.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace ch {
namespace {

// Разбор фиксированного смещения: "+03:00", "-0500", "+3".
// Возвращает false, если строка не похожа на смещение.
bool ParseFixedOffset(const std::string& value, int32_t* offsetSeconds) {
    if (value.size() < 2) return false;
    if (value[0] != '+' && value[0] != '-') return false;

    const int sign = value[0] == '-' ? -1 : 1;

    std::string digits;
    for (size_t i = 1; i < value.size(); ++i) {
        if (value[i] == ':') continue;
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
        digits.push_back(value[i]);
    }

    if (digits.empty() || digits.size() > 4) return false;

    int hours = 0;
    int minutes = 0;

    if (digits.size() <= 2) {
        hours = std::stoi(digits);
    } else {
        hours = std::stoi(digits.substr(0, digits.size() - 2));
        minutes = std::stoi(digits.substr(digits.size() - 2));
    }

    if (hours > 14 || minutes > 59) return false;

    *offsetSeconds = sign * (hours * 3600 + minutes * 60);
    return true;
}

bool EqualsIgnoreCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size(); ++i) {
        if (b[i] == '\0') return false;
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return b[i] == '\0';
}

std::tm BreakDownUtc(int64_t epochSeconds) {
    const std::time_t raw = static_cast<std::time_t>(epochSeconds);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &raw);
#else
    gmtime_r(&raw, &parts);
#endif
    return parts;
}

// Смещение локального пояса машины на ЗАДАННЫЙ момент: летнее время делает
// постоянную константу неверной полгода в году.
int32_t LocalOffsetSeconds(int64_t epochSeconds) {
    const std::time_t raw = static_cast<std::time_t>(epochSeconds);

    std::tm localParts{};
    std::tm utcParts{};
#ifdef _WIN32
    localtime_s(&localParts, &raw);
    gmtime_s(&utcParts, &raw);
#else
    localtime_r(&raw, &localParts);
    gmtime_r(&raw, &utcParts);
#endif

    // timegm/_mkgmtime трактуют tm как UTC — разность и есть смещение
#ifdef _WIN32
    const std::time_t asUtc = _mkgmtime(&localParts);
    const std::time_t asGmt = _mkgmtime(&utcParts);
#else
    const std::time_t asUtc = timegm(&localParts);
    const std::time_t asGmt = timegm(&utcParts);
#endif
    return static_cast<int32_t>(asUtc - asGmt);
}

}  // namespace

int64_t UtcNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string FormatSqlDateTime(int64_t epochSeconds) {
    const std::tm parts = BreakDownUtc(epochSeconds);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                  parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                  parts.tm_hour, parts.tm_min, parts.tm_sec);
    return std::string(buffer);
}

int32_t ResolveDisplayOffsetSeconds(const std::string& zone, ILogger* logger) {
    if (zone.empty() || EqualsIgnoreCase(zone, "UTC")) return 0;

    if (EqualsIgnoreCase(zone, "Local")) return LocalOffsetSeconds(UtcNowSeconds());

    int32_t offset = 0;
    if (ParseFixedOffset(zone, &offset)) return offset;

    if (logger != nullptr) {
        logger->Error("[Config] DisplayTimeZone \"" + zone +
                      "\" не поддержан этой сборкой: доступны \"UTC\", \"Local\" и "
                      "фиксированное смещение вида \"+03:00\". Использую \"Local\" — "
                      "пояс машины сервера. На то, что пишется в базу, настройка "
                      "не влияет: там всегда UTC");
    }
    return LocalOffsetSeconds(UtcNowSeconds());
}

std::string FormatDisplayDateTime(int64_t epochSeconds, int32_t offsetSeconds) {
    const std::tm parts = BreakDownUtc(epochSeconds + offsetSeconds);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d",
                  parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                  parts.tm_hour, parts.tm_min);
    return std::string(buffer);
}

std::string FormatDuration(int64_t seconds) {
    if (seconds < 0) seconds = 0;

    const int64_t days = seconds / 86400;
    const int64_t hours = (seconds % 86400) / 3600;
    const int64_t minutes = (seconds % 3600) / 60;
    const int64_t rest = seconds % 60;

    char buffer[64];
    if (days > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lldd %lldh %lldm",
                      static_cast<long long>(days), static_cast<long long>(hours),
                      static_cast<long long>(minutes));
    } else if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lldh %lldm",
                      static_cast<long long>(hours), static_cast<long long>(minutes));
    } else if (minutes > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lldm %llds",
                      static_cast<long long>(minutes), static_cast<long long>(rest));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llds", static_cast<long long>(rest));
    }
    return std::string(buffer);
}

}  // namespace ch
