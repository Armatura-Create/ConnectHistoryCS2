// Конфиг правит человек руками на боевом сервере, и он ошибётся в запятой.
// Битый файл не имеет права ронять плагин и не имеет права быть перезаписанным.
#include "core/config.h"
#include "core/logger.h"

#include <nlohmann/json.hpp>

#include "doctest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TempDir {
public:
    TempDir() {
        char pattern[] = "/tmp/ch_config_testXXXXXX";
        const char* made = mkdtemp(pattern);
        _path = made != nullptr ? made : "/tmp/ch_config_fallback";
    }

    ~TempDir() {
        // Каталог одноразовый; аккуратная рекурсивная чистка тут не стоит кода
        const std::string command = "rm -rf '" + _path + "'";
        if (std::system(command.c_str()) != 0) { /* мусор в /tmp безвреден */ }
    }

    const std::string& Path() const { return _path; }
    std::string File(const std::string& name) const { return _path + "/" + name; }

private:
    std::string _path;
};

bool Exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

std::string Read(const std::string& path) {
    std::ifstream stream(path.c_str(), std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void Write(const std::string& path, const std::string& content) {
    std::ofstream stream(path.c_str(), std::ios::binary | std::ios::trunc);
    stream << content;
}

}  // namespace

TEST_CASE("Первый запуск создаёт конфиги, схемы и README") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    const ch::Config config = service.LoadOrCreate(dir.Path());

    CHECK(Exists(dir.File("Settings.json")));
    CHECK(Exists(dir.File("Messages.json")));
    CHECK(Exists(dir.File("Settings.schema.json")));
    CHECK(Exists(dir.File("Messages.schema.json")));
    CHECK(Exists(dir.File("README.txt")));

    // Дефолты обезличены: первый запуск не должен ломиться в чужую базу
    CHECK(config.database.user.empty());
    CHECK(config.database.tablePrefix == "ch_");

    // Пояс отображения обязан быть виден админу в файле, а не только в схеме
    CHECK(config.displayTimeZone == "UTC");
    CHECK(Read(dir.File("Settings.json")).find("\"DisplayTimeZone\"") != std::string::npos);

    CHECK(service.FailedFiles().empty());
}

TEST_CASE("Сгенерированный конфиг ссылается на схему и остаётся читаемым") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);
    service.LoadOrCreate(dir.Path());

    const std::string settings = Read(dir.File("Settings.json"));
    CHECK(settings.find("\"$schema\": \"./Settings.schema.json\"") != std::string::npos);

    // Файл с комментариями обязан разбираться нашим же путём
    const nlohmann::json parsed =
        nlohmann::json::parse(ch::StripJsonExtras(settings), nullptr, false);
    REQUIRE_FALSE(parsed.is_discarded());
    CHECK(parsed["Database"]["Port"].get<int>() == 3306);

    // И схема, и дефолтные сообщения — валидный JSON
    CHECK_FALSE(nlohmann::json::parse(Read(dir.File("Settings.schema.json")), nullptr, false)
                    .is_discarded());
    CHECK_FALSE(nlohmann::json::parse(ch::StripJsonExtras(Read(dir.File("Messages.json"))),
                                      nullptr, false)
                    .is_discarded());
}

TEST_CASE("Битый JSON не роняет плагин и не перезаписывается") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    const std::string broken = "{ \"ServerId\": 7,, }";
    Write(dir.File("Settings.json"), broken);

    const ch::Config config = service.LoadOrCreate(dir.Path());

    // Значения по умолчанию вместо падения
    CHECK(config.serverId == 1);
    CHECK(service.FailedFiles().size() == 1);

    // Файл человека остался как был: перезаписать его значило бы уничтожить
    // работу вместе с опечаткой
    CHECK(Read(dir.File("Settings.json")) == broken);
}

TEST_CASE("Комментарии и висящие запятые принимаются") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("Settings.json"),
          "{\n"
          "  // номер этого сервера\n"
          "  \"ServerId\": 42,\n"
          "  \"Database\": {\n"
          "    \"Host\": \"db.example.com\", /* внешняя база */\n"
          "    \"TablePrefix\": \"stats_\", // с запятой на конце\n"
          "  },\n"
          "}\n");

    const ch::Config config = service.LoadOrCreate(dir.Path());

    CHECK(service.FailedFiles().empty());
    CHECK(config.serverId == 42);
    CHECK(config.database.host == "db.example.com");
    CHECK(config.database.tablePrefix == "stats_");
}

TEST_CASE("Запятая внутри строки висящей не считается") {
    // Строкоосознанность обхода: иначе ник или сообщение с ", }" превратились бы
    // в битый JSON после нашей же чистки
    const std::string text = "{ \"a\": \"привет, }\" }";
    CHECK(ch::StripJsonExtras(text) == text);

    // А комментарий внутри строки — просто текст
    const std::string url = "{ \"u\": \"https://example.com/x\" }";
    CHECK(ch::StripJsonExtras(url) == url);
}

TEST_CASE("Отсутствующие секции дают дефолты, а не нули") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("Settings.json"), "{ \"ServerId\": 3 }");

    const ch::Config config = service.LoadOrCreate(dir.Path());

    CHECK(config.serverId == 3);
    CHECK(config.database.port == 3306);
    CHECK(config.database.tablePrefix == "ch_");
    CHECK(config.storage.retryAttempts == 3);
    CHECK(config.commands.lastSeenLimit == 5);
    CHECK(config.collect.pingSampleIntervalSeconds == 30);
    // Нули здесь означали бы таймаут 0 и очередь без ретраев
    CHECK(config.database.commandTimeoutSeconds == 30);
}

TEST_CASE("Значение неверного типа не обнуляет настройку") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("Settings.json"),
          "{ \"ServerId\": \"не число\", \"Database\": { \"Port\": \"3307\" } }");

    const ch::Config config = service.LoadOrCreate(dir.Path());

    CHECK(config.serverId == 1);
    CHECK(config.database.port == 3306);
}

TEST_CASE("DisplayTimeZone читается из файла") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("Settings.json"), "{ \"DisplayTimeZone\": \"+03:00\" }");
    CHECK(service.LoadOrCreate(dir.Path()).displayTimeZone == "+03:00");
}

TEST_CASE("Settings.json не читается кем попало") {
    // В файле пароль от базы, а плагины ставят на shared-хостинг
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);
    service.LoadOrCreate(dir.Path());

    struct stat info;
    REQUIRE(stat(dir.File("Settings.json").c_str(), &info) == 0);
    CHECK((info.st_mode & S_IRGRP) == 0);
    CHECK((info.st_mode & S_IROTH) == 0);
}

TEST_CASE("Сообщения читаются в карту ключ -> язык -> текст") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    const ch::Config config = service.LoadOrCreate(dir.Path());

    REQUIRE(config.messages.count("prefix") == 1);
    CHECK(config.messages.at("prefix").count("RU") == 1);
    CHECK(config.messages.at("playtime").count("EN") == 1);
    // Теги те же, что в C#-целях: Messages.json переносится между реализациями
    CHECK(config.messages.at("prefix").at("RU").find("{GREEN}") != std::string::npos);
}

// --- Каталог конфигурации создаётся вместе с родителями ---
//
// Ровно этот случай и сломался на живом сервере: бинарник положили не в
// addons/ConnectHistory, каталога-родителя не было, одиночный mkdir падал
// с ENOENT, и конфиги не появлялись без единой строчки в консоли.

TEST_CASE("Недостающие родительские каталоги создаются") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    const std::string nested = dir.Path() + "/addons/ConnectHistory/configs";

    service.LoadOrCreate(nested);

    CHECK(Exists(nested + "/Settings.json"));
    CHECK(Exists(nested + "/Messages.json"));
    CHECK(Exists(nested + "/Settings.schema.json"));
}

TEST_CASE("Повторный запуск по существующему пути ничего не ломает") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    const std::string nested = dir.Path() + "/a/b/c";

    service.LoadOrCreate(nested);
    Write(nested + "/Settings.json", "{ \"DisplayTimeZone\": \"+05:00\" }");

    // Второй проход обязан прочитать правку человека, а не перезаписать её
    // дефолтами: каталог уже есть, и создавать заново там нечего.
    CHECK(service.LoadOrCreate(nested).displayTimeZone == "+05:00");
}

// --- Номер сервера переехал в секцию Server (3.0.1) ---
//
// Ошибиться здесь дорого: номер сервера — это ключ, по которому разделяются
// истории разных серверов. Молча сбросить его в 1 на обновлении означает
// слить их в одну, и заметно это станет только по кривому отчёту.

TEST_CASE("Номер сервера читается из секции Server") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("Settings.json"), "{ \"Server\": { \"Id\": 7 } }");

    CHECK(service.LoadOrCreate(dir.Path()).serverId == 7);
}

TEST_CASE("Конфиг до 3.0.1 с ServerId в корне продолжает работать") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("Settings.json"), "{ \"ServerId\": 4 }");

    CHECK(service.LoadOrCreate(dir.Path()).serverId == 4);
}

TEST_CASE("При обоих написаниях выигрывает Server.Id") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("Settings.json"), "{ \"ServerId\": 4, \"Server\": { \"Id\": 9 } }");

    CHECK(service.LoadOrCreate(dir.Path()).serverId == 9);
}

TEST_CASE("Конфиг по умолчанию задаёт номер сервера в новом месте") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    service.LoadOrCreate(dir.Path());

    const nlohmann::json parsed =
        nlohmann::json::parse(ch::StripJsonExtras(Read(dir.File("Settings.json"))));

    REQUIRE(parsed.contains("Server"));
    CHECK(parsed["Server"]["Id"] == 1);
    CHECK_FALSE(parsed.contains("ServerId"));
}

// --- Выбор каталога настроек (3.0.2) ---
//
// Путь переехал в addons/configs/ConnectHistory, как у большинства нативных
// плагинов CS2. Опасность переезда одна: у тех, кто уже настроил плагин,
// конфиг лежит по старому пути, и молчаливый переход означал бы выброшенные
// настройки и работу на дефолтах.

TEST_CASE("Чистая установка читает новый путь") {
    TempDir game;
    ch::NullLogger logger;

    CHECK(ch::ChooseConfigDirectory(game.Path(), &logger) ==
          game.Path() + "/addons/configs/ConnectHistory");
}

TEST_CASE("Заполненный конфиг по старому пути продолжает работать") {
    TempDir game;
    ch::NullLogger logger;

    const std::string legacy = game.Path() + "/addons/ConnectHistory/configs";
    ch::ConfigService(&logger).LoadOrCreate(legacy);

    CHECK(ch::ChooseConfigDirectory(game.Path(), &logger) == legacy);
}

TEST_CASE("Старый каталог без Settings.json не перехватывает новый путь") {
    TempDir game;
    ch::NullLogger logger;

    // Пустой каталог мог остаться от удалённой установки; он не повод
    // отправлять туда живой сервер
    const std::string legacy = game.Path() + "/addons/ConnectHistory/configs";
    const std::string command = "mkdir -p '" + legacy + "'";
    REQUIRE(std::system(command.c_str()) == 0);

    CHECK(ch::ChooseConfigDirectory(game.Path(), &logger) ==
          game.Path() + "/addons/configs/ConnectHistory");
}

TEST_CASE("Новый путь выигрывает, когда старого нет вовсе") {
    TempDir game;
    ch::NullLogger logger;

    const std::string chosen = ch::ChooseConfigDirectory(game.Path(), &logger);
    ch::ConfigService service(&logger);
    service.LoadOrCreate(chosen);

    CHECK(Exists(chosen + "/Settings.json"));
    CHECK(chosen == game.Path() + "/addons/configs/ConnectHistory");
}

// --- gamedata.json: единственные числа, зависящие от версии игры ---

TEST_CASE("Первый запуск создаёт gamedata.json со смещениями по умолчанию") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    const ch::Config config = service.LoadOrCreate(dir.Path());

    CHECK(Exists(dir.File("gamedata.json")));
    CHECK(config.gamedata.entitySystemOffsetLinux == 80);
    CHECK(config.gamedata.entitySystemOffsetWindows == 88);
}

TEST_CASE("Смещения читаются из gamedata.json и файл не перезаписывается") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    // Файл с комментарием, как его напишет человек после обновления игры
    const std::string edited =
        "{ // после обновления 2026-10-01\n  \"GameEntitySystem\": { \"linux\": 96, \"windows\": 104 } }";
    Write(dir.File("gamedata.json"), edited);

    const ch::Config config = service.LoadOrCreate(dir.Path());

    CHECK(config.gamedata.entitySystemOffsetLinux == 96);
    CHECK(config.gamedata.entitySystemOffsetWindows == 104);
    CHECK(Read(dir.File("gamedata.json")) == edited);
}

TEST_CASE("Битый gamedata.json оставляет смещения по умолчанию и не перезаписывается") {
    TempDir dir;
    ch::NullLogger logger;
    ch::ConfigService service(&logger);

    Write(dir.File("gamedata.json"), "{ \"GameEntitySystem\": { \"linux\": 96, }");  // }
    const std::string broken = Read(dir.File("gamedata.json"));

    const ch::Config config = service.LoadOrCreate(dir.Path());

    CHECK(config.gamedata.entitySystemOffsetLinux == 80);
    CHECK(Read(dir.File("gamedata.json")) == broken);
    CHECK(std::find(service.FailedFiles().begin(), service.FailedFiles().end(),
                    "gamedata.json") != service.FailedFiles().end());
}
