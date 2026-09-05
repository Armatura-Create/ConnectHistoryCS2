// Настоящие базы GeoLite2 из репозитория, открытые ИЗ ПАМЯТИ.
//
// Ради этого теста и написан ch_mmdb_open_memory: штатный MMDB_open делает mmap,
// а страничный отказ внутри игрового процесса — это SIGBUS и мгновенная смерть
// сервера без строки в логе. Инцидент, из-за которого это правило появилось,
// случился на полностью целом файле — на overlayfs в Docker.
#include "core/geoip.h"
#include "core/logger.h"

#include "doctest.h"

#include <string>
#include <sys/stat.h>

namespace {

// Каталог с базами: репозиторий держит одну копию на все три цели.
// Тест запускается из metamod/, поэтому путь относительный.
const char* kGeoDirectory = "../GeoIP";

bool DatabasesPresent() {
    struct stat info;
    return stat((std::string(kGeoDirectory) + "/GeoLite2-Country.mmdb").c_str(), &info) == 0;
}

}  // namespace

TEST_CASE("Базы GeoLite2 открываются из памяти и отвечают") {
    if (!DatabasesPresent()) {
        MESSAGE("GeoIP/*.mmdb нет — проверка пропущена");
        return;
    }

    ch::NullLogger logger;
    ch::GeoIpService geoip(kGeoDirectory, &logger);

    const ch::GeoInfo google = geoip.Lookup("8.8.8.8");
    CHECK(google.hasIso);
    CHECK(google.iso == "US");
    CHECK(google.hasCountry);

    // Повторный поиск идёт по уже открытой базе — второй раз файл не читается
    const ch::GeoInfo again = geoip.Lookup("8.8.8.8");
    CHECK(again.iso == "US");
}

TEST_CASE("Приватные адреса до базы не доходят") {
    // У них нет географии, и обращаться к базе незачем
    ch::NullLogger logger;
    ch::GeoIpService geoip(kGeoDirectory, &logger);

    CHECK_FALSE(geoip.Lookup("192.168.1.10").hasIso);
    CHECK_FALSE(geoip.Lookup("127.0.0.1").hasIso);
    CHECK_FALSE(geoip.Lookup("10.0.0.1").hasIso);
    CHECK_FALSE(geoip.Lookup("").hasIso);
    CHECK_FALSE(geoip.Lookup("не адрес").hasIso);
}

TEST_CASE("Отсутствие базы — не ошибка, а отсутствие данных") {
    // Плагин обязан работать без .mmdb: страна просто не определится
    ch::NullLogger logger;
    ch::GeoIpService geoip("/nonexistent-directory-for-tests", &logger);

    const ch::GeoInfo info = geoip.Lookup("8.8.8.8");
    CHECK_FALSE(info.hasIso);
    CHECK_FALSE(info.hasCity);
}

TEST_CASE("Закрытие идемпотентно") {
    ch::NullLogger logger;
    ch::GeoIpService geoip(kGeoDirectory, &logger);
    geoip.Lookup("8.8.8.8");

    geoip.Close();
    geoip.Close();  // повторное закрытие не должно ронять процесс
}
