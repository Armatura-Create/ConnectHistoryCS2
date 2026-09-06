// Конфигурация плагина. Формат файлов совпадает с C#-целями: Settings.json
// и Messages.json переносятся между реализациями без правок.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ch {

class ILogger;

struct ServerSettings {
    // Публичный адрес сервера в виде "ip:port" или "host:port".
    //
    // Зачем настройка, а не автоопределение: процесс игрового сервера НЕ ЗНАЕТ
    // своего публичного адреса. ConVar ip отдаёт адрес привязки сокета, и при
    // обычной конфигурации это 0.0.0.0 — «слушаю все интерфейсы». Записать его
    // означает записать значение, по которому нельзя подключиться.
    std::string publicAddress;
};

struct DatabaseSettings {
    std::string host = "127.0.0.1";
    uint32_t port = 3306;
    std::string database = "connect_history";
    std::string user;
    std::string password;

    // None | Preferred | Required. Required — разумный минимум для базы за пределами
    // localhost: без него ник, IP и SteamID идут открытым текстом.
    std::string sslMode = "Preferred";

    // Позволяет держать несколько проектов в одной базе.
    std::string tablePrefix = "ch_";

    uint32_t connectionTimeoutSeconds = 10;
    uint32_t commandTimeoutSeconds = 30;
};

struct CollectSettings {
    bool geoIp = true;
    bool playerIp = true;
    bool ipHash = true;
    // Пустая соль = хеш обратим перебором IPv4 за минуты, поэтому при пустом
    // значении хеширование выключается.
    std::string ipHashSalt;
    bool matchStats = true;
    bool ping = true;
    // Считать ли в «наиграно» время наблюдателя. На sessions.duration_seconds
    // не влияет: там всегда честное время подключения.
    bool countSpectatorTime = true;
    bool nicknameHistory = true;
    bool playerAggregates = true;
    bool onlineSnapshots = true;
    int32_t onlineSnapshotIntervalSeconds = 300;
    int32_t pingSampleIntervalSeconds = 30;
};

struct StorageSettings {
    // Без спула падение базы на десять минут = потерянные сессии.
    bool spoolEnabled = true;
    int32_t spoolMaxEntries = 20000;
    int32_t retryAttempts = 3;
    int32_t retryDelaySeconds = 5;
};

struct CommandsSettings {
    bool playerCommandsEnabled = true;
    int32_t cooldownSeconds = 10;
    int32_t lastSeenLimit = 5;
};

struct Config {
    bool debug = false;
    int32_t serverId = 1;
    std::string defaultLang = "RU";
    // "UTC", "Local" или фиксированное смещение "+03:00". На базу не влияет.
    std::string displayTimeZone = "UTC";

    ServerSettings server;
    DatabaseSettings database;
    CollectSettings collect;
    StorageSettings storage;
    CommandsSettings commands;

    // ключ -> язык -> текст
    std::map<std::string, std::map<std::string, std::string> > messages;
};

// Убирает из JSON то, что человек пишет руками, а строгий парсер не принимает:
// //-комментарии, /* */ и висящие запятые перед } и ].
//
// Разрешены сознательно: это самые частые «ошибки» в правленом руками конфиге,
// а читаются такие файлы однозначно. Обход строкоосознанный — запятая внутри
// строкового литерала висящей не считается.
std::string StripJsonExtras(const std::string& text);

// Загрузка конфигурации из каталога.
//
// Битый файл НЕ роняет плагин и НЕ перезаписывается: в лог уходит имя файла
// и позиция ошибки, а значения берутся по умолчанию. Перезаписать испорченный
// руками конфиг значит уничтожить работу человека вместе с его опечаткой.
class ConfigService {
public:
    explicit ConfigService(ILogger* logger) : _logger(logger) {}

    // Каталог, из которого прочитан конфиг. Нужен командам для подсказки админу.
    const std::string& Directory() const { return _directory; }

    // Файлы, которые не удалось разобрать в последний раз.
    const std::vector<std::string>& FailedFiles() const { return _failedFiles; }

    Config LoadOrCreate(const std::string& configDirectory);

private:
    ILogger* _logger;
    std::string _directory;
    std::vector<std::string> _failedFiles;
};

// Текст файлов по умолчанию — вынесены, чтобы их проверял тест.
std::string DefaultSettingsJson();
std::string DefaultMessagesJson();
std::string SettingsSchemaJson();
std::string MessagesSchemaJson();
std::string ReadmeText();

}  // namespace ch
