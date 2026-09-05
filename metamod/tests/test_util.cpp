// Те же проверки, что в C#-целях. Инварианты, а не реализация: обе стороны
// пишут в одни колонки, и разъехавшееся поведение обнаружится не тестом,
// а отчётом, который перестанет сходиться.
#include "core/logger.h"
#include "core/util/chat_format.h"
#include "core/util/ip.h"
#include "core/util/sql_sanitizer.h"
#include "core/util/steamid.h"
#include "core/util/timeutil.h"

#include "doctest.h"

TEST_CASE("ExtractIp понимает IPv4 и IPv6") {
    CHECK(ch::ip::ExtractIp("1.2.3.4:27015") == "1.2.3.4");
    CHECK(ch::ip::ExtractIp("1.2.3.4") == "1.2.3.4");
    CHECK(ch::ip::ExtractIp("[2001:db8::1]:27015") == "2001:db8::1");
    // Голый IPv6: наивный split по ':' его ломал
    CHECK(ch::ip::ExtractIp("2001:db8::1") == "2001:db8::1");
    CHECK(ch::ip::ExtractIp("") == "");
}

TEST_CASE("Подсеть отбрасывает хостовую часть") {
    CHECK(ch::ip::ToSubnet("203.0.113.77") == "203.0.113.0/24");
    CHECK(ch::ip::ToSubnet("2001:db8:1234:5678::1") == "2001:db8:1234::/48");
    CHECK(ch::ip::ToSubnet("not-an-ip") == "");
}

TEST_CASE("Хеш IP стабилен и зависит от соли") {
    const std::string a = ch::ip::Hash("203.0.113.77", "salt-one");
    const std::string b = ch::ip::Hash("203.0.113.77", "salt-one");
    const std::string c = ch::ip::Hash("203.0.113.77", "salt-two");

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.size() == 64);  // CHAR(64) в схеме
}

TEST_CASE("Без соли хеширование выключено") {
    // Хеш без соли обратим перебором всего IPv4 за минуты — такая колонка
    // притворялась бы анонимной
    CHECK(ch::ip::Hash("203.0.113.77", "") == "");
}

TEST_CASE("Приватные диапазоны распознаются") {
    CHECK(ch::ip::IsLocalOrPrivate("10.1.2.3"));
    CHECK(ch::ip::IsLocalOrPrivate("192.168.0.10"));
    CHECK(ch::ip::IsLocalOrPrivate("172.16.5.5"));
    CHECK(ch::ip::IsLocalOrPrivate("127.0.0.1"));
    CHECK(ch::ip::IsLocalOrPrivate("::1"));
    CHECK(ch::ip::IsLocalOrPrivate("fe80::1"));
    CHECK(ch::ip::IsLocalOrPrivate("fd00::1"));
    CHECK_FALSE(ch::ip::IsLocalOrPrivate("8.8.8.8"));
    CHECK(ch::ip::IsLocalOrPrivate("garbage"));
}

TEST_CASE("Настройка адреса важнее автоопределения") {
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10:27015", "10.0.0.5", 27016) ==
          "203.0.113.10:27015");
    CHECK(ch::ip::ResolvePublicAddress("", "203.0.113.10", 27015) == "203.0.113.10:27015");
}

TEST_CASE("Адрес привязки не записывается вместо адреса сервера") {
    CHECK(ch::ip::ResolvePublicAddress("", "0.0.0.0", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("", "::", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("", "127.0.0.1", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("", "10.0.0.5", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("", "192.168.1.10", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("", "169.254.1.1", 27015) == "");
}

TEST_CASE("Бесполезная настройка отбраковывается") {
    CHECK(ch::ip::ResolvePublicAddress("0.0.0.0:27015", "203.0.113.10", 0) == "");
    CHECK(ch::ip::ResolvePublicAddress("0.0.0.0", "203.0.113.10", 0) == "");
    CHECK(ch::ip::ResolvePublicAddress("127.0.0.1:27015", "203.0.113.10", 0) == "");
    CHECK(ch::ip::ResolvePublicAddress("192.168.0.10:27015", "203.0.113.10", 0) == "");
}

TEST_CASE("Заполненная, но непригодная настройка не подменяется автоопределением") {
    // Иначе в базе окажется адрес, которого человек не писал, а опечатка
    // так и останется незамеченной
    CHECK(ch::ip::ResolvePublicAddress(":", "203.0.113.10", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress(":27015", "203.0.113.10", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10:0", "203.0.113.10", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10:70000", "203.0.113.10", 27015) == "");
}

TEST_CASE("Пробелы — это «не заполнено», а не «заполнено неверно»") {
    CHECK(ch::ip::ResolvePublicAddress("   ", "203.0.113.10", 27015) == "203.0.113.10:27015");
}

TEST_CASE("Опечатка в порту — не то же самое, что отсутствие порта") {
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10:порт", "203.0.113.10", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10:", "203.0.113.10", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10:+27015", "203.0.113.10", 27015) == "");
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10: 27015", "203.0.113.10", 27015) == "");
}

TEST_CASE("Порт берётся из ConVar, если в настройке его нет") {
    CHECK(ch::ip::ResolvePublicAddress("203.0.113.10", "0.0.0.0", 27015) == "203.0.113.10:27015");
}

TEST_CASE("Домен — законный публичный адрес") {
    CHECK(ch::ip::ResolvePublicAddress("cs2.example.com:27015", "", 0) == "cs2.example.com:27015");
}

TEST_CASE("IPv6 всегда в скобках") {
    CHECK(ch::ip::ResolvePublicAddress("[2001:db8::1]:27015", "", 27015) == "[2001:db8::1]:27015");
    CHECK(ch::ip::ResolvePublicAddress("2001:db8::1", "", 27015) == "[2001:db8::1]:27015");
}

TEST_CASE("Нулевой порт без настройки даёт пусто") {
    CHECK(ch::ip::ResolvePublicAddress("", "203.0.113.10", 0) == "");
}

TEST_CASE("Префикс таблиц проходит белый список") {
    CHECK(ch::SanitizePrefix("ch_") == "ch_");
    CHECK(ch::SanitizePrefix("") == "");
    CHECK(ch::SanitizePrefix("stats2_") == "stats2_");
    CHECK(ch::SanitizePrefix("ch`; DROP TABLE users; --") == "ch_");
    CHECK(ch::SanitizePrefix("ch-prefix") == "ch_");
    CHECK(ch::SanitizePrefix("way_too_long_prefix_value") == "ch_");
}

TEST_CASE("Префикс с переводом строки отбраковывается") {
    // В C#-целях это была настоящая дыра: якорь $ в регулярке совпадает ПЕРЕД
    // завершающим \n, и "ch_\n" проезжал проверку, уезжая в текст SQL
    CHECK(ch::SanitizePrefix("ch_\n") == "ch_");
    CHECK(ch::SanitizePrefix("ch_\r\n") == "ch_");
    CHECK(ch::SanitizePrefix("ch_\n; DROP TABLE ch_sessions; --") == "ch_");
}

TEST_CASE("Пароль не уходит в лог") {
    const std::string masked =
        ch::MaskSecrets("Access denied (server=db;pwd=hunter2;db=x)");
    CHECK(masked.find("hunter2") == std::string::npos);
    CHECK(masked.find("password=***") != std::string::npos);
    CHECK(masked.find("db=x") != std::string::npos);

    const std::string full = ch::MaskSecrets(
        "host=db;port=3306;user=ch;Password=super-secret;database=ch");
    CHECK(full.find("super-secret") == std::string::npos);
    CHECK(full.find("database=ch") != std::string::npos);

    // Слово "password" внутри другого слова паролем не является
    CHECK(ch::MaskSecrets("mypassword=abc").find("mypassword=abc") != std::string::npos);
}

TEST_CASE("Длительность форматируется одинаково на любой локали") {
    CHECK(ch::FormatDuration(7500) == "2h 5m");
    CHECK(ch::FormatDuration(97200) == "1d 3h 0m");
    CHECK(ch::FormatDuration(-5) == "0s");
    CHECK(ch::FormatDuration(45) == "45s");
    CHECK(ch::FormatDuration(125) == "2m 5s");
}

TEST_CASE("Подстановка значений и цветовых тегов") {
    std::map<std::string, std::string> values;
    values["{NAME}"] = "Игрок";

    const std::string rendered = ch::chat::Render("{GREEN}Привет, {NAME}{DEFAULT}", values);

    CHECK(rendered.find("Игрок") != std::string::npos);
    CHECK(rendered.find("{GREEN}") == std::string::npos);
    CHECK(rendered.find("{DEFAULT}") == std::string::npos);
    CHECK(rendered.find('\x04') != std::string::npos);
}

TEST_CASE("Цвет в самом начале сообщения получает префикс") {
    const std::string result = ch::chat::EnsureChatColorPrefix("\x04[История]");

    CHECK(result.substr(0, 2) == "\x01 ");
    CHECK(result.find("\x04[История]") != std::string::npos);
}

TEST_CASE("Починка префикса идемпотентна") {
    const std::string once = ch::chat::EnsureChatColorPrefix("\x04Текст");
    CHECK(ch::chat::EnsureChatColorPrefix(once) == once);

    CHECK(ch::chat::EnsureChatColorPrefix(" \x04Текст") == "\x01 \x04Текст");
    // Текст без цветов не трогаем — иначе к обычному сообщению добавится пробел
    CHECK(ch::chat::EnsureChatColorPrefix("просто текст") == "просто текст");
}

TEST_CASE("SteamID: боты и невалидные контроллеры в историю не попадают") {
    CHECK_FALSE(ch::IsRealSteamId(0));
    CHECK_FALSE(ch::IsRealSteamId(76561197960265728ull));
    CHECK(ch::IsRealSteamId(76561198000000000ull));
}

TEST_CASE("account_id не переполняется на современных аккаунтах") {
    // За 2^31 такой id в signed int становится отрицательным
    const uint64_t steamId = 76561200460265728ull;
    const uint32_t accountId = ch::ToAccountId(steamId);

    CHECK(accountId > 2147483647u);
    CHECK(accountId == static_cast<uint32_t>(steamId - 76561197960265728ull));
}

TEST_CASE("Время в базу уходит в UTC") {
    // 2026-09-05 12:34:56 UTC
    CHECK(ch::FormatSqlDateTime(1788611696) == "2026-09-05 12:34:56");
}

TEST_CASE("Пояс отображения: UTC, Local и фиксированное смещение") {
    ch::NullLogger logger;

    CHECK(ch::ResolveDisplayOffsetSeconds("UTC", &logger) == 0);
    CHECK(ch::ResolveDisplayOffsetSeconds("utc", &logger) == 0);
    CHECK(ch::ResolveDisplayOffsetSeconds("+03:00", &logger) == 3 * 3600);
    CHECK(ch::ResolveDisplayOffsetSeconds("-05:30", &logger) == -(5 * 3600 + 30 * 60));
    CHECK(ch::ResolveDisplayOffsetSeconds("+0200", &logger) == 2 * 3600);

    // Пояс влияет только на показ, но показывать обязан верно
    CHECK(ch::FormatDisplayDateTime(1788611696, 3 * 3600) == "2026-09-05 15:34");
    CHECK(ch::FormatDisplayDateTime(1788611696, 0) == "2026-09-05 12:34");
}
