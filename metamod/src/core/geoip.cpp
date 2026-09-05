#include "core/geoip.h"

#include "core/geoip_memory.h"
#include "core/logger.h"
#include "core/util/ip.h"

#include <cstdio>
#include <sys/stat.h>

namespace ch {

struct GeoIpService::Database {
    MMDB_s handle{};
    bool open = false;
    bool tried = false;
};

namespace {

// Значение строкового поля из результата поиска. Пустая строка означает
// «поля нет» — для GeoLite2 это обычное дело у мелких стран и у городов.
bool ReadString(MMDB_lookup_result_s* result, std::string* out, const char* first,
                const char* second, const char* third) {
    MMDB_entry_data_s data;
    const int status = third == nullptr
                           ? MMDB_get_value(&result->entry, &data, first, second, NULL)
                           : MMDB_get_value(&result->entry, &data, first, second, third, NULL);

    if (status != MMDB_SUCCESS || !data.has_data ||
        data.type != MMDB_DATA_TYPE_UTF8_STRING) {
        return false;
    }

    out->assign(data.utf8_string, data.data_size);
    return !out->empty();
}

int64_t FileSize(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return -1;
    return static_cast<int64_t>(info.st_size);
}

}  // namespace

GeoIpService::~GeoIpService() { Close(); }

void GeoIpService::Close() {
    std::lock_guard<std::mutex> guard(_mutex);

    Database** slots[] = {&_country, &_city};
    for (Database** slot : slots) {
        if (*slot == nullptr) continue;
        if ((*slot)->open) ch_mmdb_close_memory(&(*slot)->handle);
        delete *slot;
        *slot = nullptr;
    }
}

// Открывает базу MaxMind, печатая путь и РАЗМЕР файла.
//
// Размер в логе не для красоты: базы качаются на этапе сборки, и недокачанный
// файл выглядит как обычный — до первого чтения. Ориентиры: Country ~9 МБ,
// City ~60 МБ.
GeoIpService::Database* GeoIpService::Ensure(Database** slot, const char* fileName) {
    if (*slot != nullptr) return (*slot)->open ? *slot : nullptr;

    Database* database = new Database();
    *slot = database;
    database->tried = true;

    const std::string path = _directory.empty() ? std::string(fileName)
                                                : _directory + "/" + fileName;

    const int64_t size = FileSize(path);
    if (size < 0) {
        if (_logger != nullptr) {
            _logger->Warn(std::string("[GEO] база ") + fileName + " не найдена (" + path +
                          ") — страна и город собираться не будут");
        }
        return nullptr;
    }

    // ch_mmdb_open_memory, а НЕ MMDB_open: штатное открытие делает mmap, а
    // страничный отказ внутри игрового процесса — это SIGBUS и мгновенная
    // смерть сервера без стека в логе.
    const int status = ch_mmdb_open_memory(path.c_str(), &database->handle);
    if (status != MMDB_SUCCESS) {
        if (_logger != nullptr) {
            _logger->Error(std::string("[GEO] не удалось открыть ") + fileName + ": " +
                           MMDB_strerror(status));
        }
        return nullptr;
    }

    database->open = true;
    if (_logger != nullptr) {
        _logger->Info(std::string("[GEO] ") + fileName + " загружена в память (" +
                      std::to_string(size) + " байт)");
    }
    return database;
}

GeoInfo GeoIpService::Lookup(const std::string& ip) {
    GeoInfo info;

    if (ip.empty() || ip::IsLocalOrPrivate(ip)) return info;

    std::lock_guard<std::mutex> guard(_mutex);

    Database* country = Ensure(&_country, "GeoLite2-Country.mmdb");
    if (country != nullptr) {
        int gai = 0;
        int mmdb = MMDB_SUCCESS;
        MMDB_lookup_result_s result =
            MMDB_lookup_string(&country->handle, ip.c_str(), &gai, &mmdb);

        if (gai == 0 && mmdb == MMDB_SUCCESS && result.found_entry) {
            info.hasIso = ReadString(&result, &info.iso, "country", "iso_code", nullptr);
            info.hasCountry =
                ReadString(&result, &info.country, "country", "names", "en");
        }
    }

    Database* city = Ensure(&_city, "GeoLite2-City.mmdb");
    if (city != nullptr) {
        int gai = 0;
        int mmdb = MMDB_SUCCESS;
        MMDB_lookup_result_s result =
            MMDB_lookup_string(&city->handle, ip.c_str(), &gai, &mmdb);

        if (gai == 0 && mmdb == MMDB_SUCCESS && result.found_entry) {
            info.hasCity = ReadString(&result, &info.city, "city", "names", "en");

            // База стран могла отсутствовать — City знает страну тоже
            if (!info.hasIso) {
                info.hasIso = ReadString(&result, &info.iso, "country", "iso_code", nullptr);
            }
            if (!info.hasCountry) {
                info.hasCountry = ReadString(&result, &info.country, "country", "names", "en");
            }
        }
    }

    return info;
}

}  // namespace ch
