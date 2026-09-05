// Страна и город по IP. Базы GeoLite2 лежат рядом с плагином.
#pragma once

#include <mutex>
#include <string>

namespace ch {

class ILogger;

struct GeoInfo {
    // Всё необязательно: отсутствие базы, приватный адрес и незнакомый диапазон —
    // нормальные ситуации, а не ошибки.
    bool hasIso = false;
    std::string iso;
    bool hasCountry = false;
    std::string country;
    bool hasCity = false;
    std::string city;
};

class GeoIpService {
public:
    GeoIpService(const std::string& moduleDirectory, ILogger* logger)
        : _directory(moduleDirectory), _logger(logger) {}

    ~GeoIpService();

    GeoIpService(const GeoIpService&) = delete;
    GeoIpService& operator=(const GeoIpService&) = delete;

    GeoInfo Lookup(const std::string& ip);

    void Close();

private:
    // База открывается лениво и ровно один раз: неудачную попытку не повторяем,
    // иначе каждый заход игрока стучался бы в отсутствующий файл.
    struct Database;

    Database* Ensure(Database** slot, const char* fileName);

    std::string _directory;
    ILogger* _logger;
    std::mutex _mutex;

    Database* _country = nullptr;
    Database* _city = nullptr;
};

}  // namespace ch
