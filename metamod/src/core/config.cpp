#include "core/config.h"

#include "core/logger.h"
#include "core/util/fs.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace ch {
namespace {

using nlohmann::json;

bool ReadWholeFile(const std::string& path, std::string* out) {
    std::ifstream stream(path.c_str(), std::ios::binary);
    if (!stream.is_open()) return false;

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    *out = buffer.str();
    return true;
}

bool WriteWholeFile(const std::string& path, const std::string& content) {
    std::ofstream stream(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) return false;

    stream << content;
    return stream.good();
}

bool FileExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

// Settings.json содержит пароль от базы: на shared-хостинге его не должен читать
// кто попало. На Windows прав POSIX нет — там это просто не применяется.
void ProtectSecrets(const std::string& path) {
#ifndef _WIN32
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#else
    (void)path;
#endif
}

// Следующий значимый символ после позиции: пробелы и комментарии пропускаются.
// '\0' означает «дальше ничего нет».
char NextMeaningfulChar(const std::string& text, size_t from) {
    size_t i = from;
    while (i < text.size()) {
        const char c = text[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }

        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }

        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }

        return c;
    }
    return '\0';
}

std::string Join(const std::string& directory, const std::string& name) {
    if (directory.empty()) return name;
    const char last = directory[directory.size() - 1];
    if (last == '/' || last == '\\') return directory + name;
    return directory + "/" + name;
}

// Чтение БЕЗ исключений.
//
// Не стилистика: игровая сборка идёт с -fno-exceptions (так собран весь hl2sdk),
// и в таком режиме nlohmann/json на ошибке зовёт std::abort(). То есть кривой
// Settings.json убивал бы сервер вместо отката на значения по умолчанию — ровно
// наоборот тому, что обещает ConfigResilienceTests. Поэтому тип проверяется
// заранее, а get<T> вызывается только когда он заведомо подойдёт.
void ReadValue(const json& value, bool* target) {
    if (value.is_boolean()) *target = value.get<bool>();
}

void ReadValue(const json& value, int32_t* target) {
    if (value.is_number_integer()) *target = static_cast<int32_t>(value.get<int64_t>());
}

void ReadValue(const json& value, uint32_t* target) {
    if (!value.is_number_unsigned()) return;
    const uint64_t raw = value.get<uint64_t>();
    if (raw <= 0xFFFFFFFFull) *target = static_cast<uint32_t>(raw);
}

void ReadValue(const json& value, std::string* target) {
    if (value.is_string()) *target = value.get<std::string>();
}

// Читает поле, оставляя значение по умолчанию, если его нет или тип не тот.
// Частичный или битый файл не должен приводить к нулям в базе.
template <typename T>
void Read(const json& node, const char* key, T* target) {
    if (!node.is_object()) return;
    const auto it = node.find(key);
    if (it == node.end() || it->is_null()) return;

    ReadValue(*it, target);
}

const json& Section(const json& root, const char* key, const json& fallback) {
    if (!root.is_object()) return fallback;
    const auto it = root.find(key);
    if (it == root.end() || !it->is_object()) return fallback;
    return *it;
}

}  // namespace

std::string StripJsonExtras(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    bool inString = false;
    bool escaped = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (inString) {
            out.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            out.push_back(c);
            continue;
        }

        // //-комментарий до конца строки
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            if (i < text.size()) out.push_back('\n');
            continue;
        }

        // /* */ — переводы строк внутри сохраняем, иначе поедут номера строк
        // в сообщении об ошибке разбора, а именно ими человек и чинит конфиг
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                if (text[i] == '\n') out.push_back('\n');
                ++i;
            }
            ++i;
            continue;
        }

        if (c == ',') {
            // Висящая запятая: следующий значимый символ — закрывающая скобка.
            // Заглядывать нужно СКВОЗЬ пробелы и комментарии: "1, // прим.\n}"
            // человек пишет ровно так же часто, как "1,\n}"
            const char next = NextMeaningfulChar(text, i + 1);
            if (next == '}' || next == ']') continue;
        }

        out.push_back(c);
    }

    return out;
}

std::string ChooseConfigDirectory(const std::string& gameDirectory, ILogger* logger) {
    const std::string current = gameDirectory + "/addons/configs/ConnectHistory";
    const std::string legacy = gameDirectory + "/addons/ConnectHistory/configs";

    if (FileExists(Join(legacy, "Settings.json"))) {
        if (logger != nullptr) {
            logger->Warn("[Config] Настройки найдены по старому пути: " + legacy);
            logger->Warn("[Config] Перенесите их в " + current +
                         " — старый путь поддерживается, но однажды исчезнет");
        }
        return legacy;
    }

    return current;
}

// Проверка конфигурации.
//
// Существует потому, что молчаливая ошибка настройки выглядит как поломка чего-то
// другого. Незаполненная секция Database даёт не "заполните конфиг", а таймаут
// подключения к 127.0.0.1 — и владелец сервера идёт чинить MySQL, который не при
// чём. Такой же блок есть в C#-целях; при портировании на C++ он потерялся.
void ConfigService::Validate(const Config& config, const std::string& directory) const {
    if (_logger == nullptr) return;

    if (config.database.host.empty() || config.database.database.empty() ||
        config.database.user.empty()) {
        _logger->Error("[Config] Секция Database заполнена не полностью (Host/Database/User). "
                       "История подключений собираться не будет");
        _logger->Error("[Config] Проверьте " + Join(directory, "Settings.json") +
                       " — плагин читает ИМЕННО этот файл");
    }

    if (config.serverId <= 0) {
        _logger->Warn("[Config] Server.Id <= 0. Разным серверам нужны разные номера, "
                      "иначе их сессии смешаются в одну кучу");
    }

    if (config.collect.ipHash && config.collect.ipHashSalt.empty()) {
        _logger->Warn("[Config] Collect.IpHash включён, но IpHashSalt пуст. "
                      "Хеш без соли обратим перебором IPv4 за минуты — хеширование отключено");
    }

    if (!config.collect.playerIp && !config.collect.ipHash && !config.collect.geoIp) {
        _logger->Info("[Config] Сбор сетевых данных полностью отключён");
    }

    const bool localDb = config.database.host == "127.0.0.1" ||
                         config.database.host == "localhost" || config.database.host == "::1";
    if (!localDb && config.database.sslMode == "None") {
        _logger->Warn("[Config] SslMode=None при удалённой базе: ники, SteamID и IP игроков "
                      "идут по сети открытым текстом. Рекомендуется Required");
    }

    // Печатается ВСЕГДА, а не только на первом запуске: без этой строки нельзя
    // отличить "конфиг не заполнен" от "заполнен не тот файл", а это разные
    // починки. Ровно на этом и застревали.
    _logger->Info("[Config] Конфигурация загружена из " + directory);
}

Config ConfigService::LoadOrCreate(const std::string& configDirectory) {
    _failedFiles.clear();
    _directory = configDirectory;

    const bool haveDirectory = fs::EnsureDirectory(configDirectory);
    if (!haveDirectory && _logger != nullptr) {
        // Молчать здесь нельзя: плагин продолжит работать на значениях по
        // умолчанию, то есть без базы, и админ будет искать причину в MySQL.
        _logger->Error("[Config] не удалось создать каталог конфигурации: " + configDirectory);
        _logger->Error("[Config] распакуйте configs/ из архива по этому пути "
                       "или дайте серверу право писать туда");
    }

    const std::string settingsPath = Join(configDirectory, "Settings.json");
    const std::string messagesPath = Join(configDirectory, "Messages.json");

    // Схемы и README перезаписываются ВСЕГДА: иначе после обновления плагина они
    // продолжают описывать старую версию и врут админу. Справочные файлы — не повод
    // сорвать загрузку конфига: каталог может быть только для чтения.
    if (haveDirectory) {
        WriteWholeFile(Join(configDirectory, "Settings.schema.json"), SettingsSchemaJson());
        WriteWholeFile(Join(configDirectory, "Messages.schema.json"), MessagesSchemaJson());
        WriteWholeFile(Join(configDirectory, "README.txt"), ReadmeText());
    }

    if (!FileExists(settingsPath)) {
        if (_logger != nullptr) {
            _logger->Info("═══════════════════════════════════════════════════════════════");
            _logger->Info("  ConnectHistory: первый запуск — создаю файлы конфигурации");
            _logger->Info("  " + configDirectory);
            _logger->Info("═══════════════════════════════════════════════════════════════");
        }
        if (!WriteWholeFile(settingsPath, DefaultSettingsJson()) && _logger != nullptr) {
            _logger->Error("[Config] не удалось записать " + settingsPath);
            _logger->Error("[Config] плагин работает на значениях по умолчанию: "
                           "база не настроена, в неё ничего не пишется");
        }
    }
    ProtectSecrets(settingsPath);

    if (!FileExists(messagesPath)) {
        if (!WriteWholeFile(messagesPath, DefaultMessagesJson()) && _logger != nullptr) {
            _logger->Error("[Config] не удалось записать " + messagesPath);
        }
    }

    Config config;

    std::string raw;
    if (ReadWholeFile(settingsPath, &raw)) {
        json parsed = json::parse(StripJsonExtras(raw), nullptr, false);
        if (parsed.is_discarded()) {
            _failedFiles.push_back("Settings.json");
            if (_logger != nullptr) {
                _logger->Error("[Config] Settings.json разобрать не удалось — беру значения "
                               "по умолчанию. Файл НЕ перезаписан: почините запятую и "
                               "выполните ch_reload");
            }
        } else {
            Read(parsed, "Debug", &config.debug);
            Read(parsed, "DefaultLang", &config.defaultLang);
            Read(parsed, "DisplayTimeZone", &config.displayTimeZone);

            const json empty = json::object();

            // Номер сервера жил в корне конфига до 3.0.1. Старое место читается
            // ПЕРВЫМ, а Server.Id перекрывает его: иначе обновление плагина
            // тихо сбросило бы номер в 1 и слило историю двух серверов в одну.
            Read(parsed, "ServerId", &config.serverId);

            const json& server = Section(parsed, "Server", empty);
            Read(server, "Id", &config.serverId);
            Read(server, "PublicAddress", &config.server.publicAddress);

            const json& database = Section(parsed, "Database", empty);
            Read(database, "Host", &config.database.host);
            Read(database, "Port", &config.database.port);
            Read(database, "Database", &config.database.database);
            Read(database, "User", &config.database.user);
            Read(database, "Password", &config.database.password);
            Read(database, "SslMode", &config.database.sslMode);
            Read(database, "TablePrefix", &config.database.tablePrefix);
            Read(database, "ConnectionTimeoutSeconds", &config.database.connectionTimeoutSeconds);
            Read(database, "CommandTimeoutSeconds", &config.database.commandTimeoutSeconds);

            const json& collect = Section(parsed, "Collect", empty);
            Read(collect, "GeoIp", &config.collect.geoIp);
            Read(collect, "PlayerIp", &config.collect.playerIp);
            Read(collect, "IpHash", &config.collect.ipHash);
            Read(collect, "IpHashSalt", &config.collect.ipHashSalt);
            Read(collect, "MatchStats", &config.collect.matchStats);
            Read(collect, "Ping", &config.collect.ping);
            Read(collect, "CountSpectatorTime", &config.collect.countSpectatorTime);
            Read(collect, "NicknameHistory", &config.collect.nicknameHistory);
            Read(collect, "PlayerAggregates", &config.collect.playerAggregates);
            Read(collect, "OnlineSnapshots", &config.collect.onlineSnapshots);
            Read(collect, "OnlineSnapshotIntervalSeconds",
                 &config.collect.onlineSnapshotIntervalSeconds);
            Read(collect, "PingSampleIntervalSeconds", &config.collect.pingSampleIntervalSeconds);

            const json& storage = Section(parsed, "Storage", empty);
            Read(storage, "SpoolEnabled", &config.storage.spoolEnabled);
            Read(storage, "SpoolMaxEntries", &config.storage.spoolMaxEntries);
            Read(storage, "RetryAttempts", &config.storage.retryAttempts);
            Read(storage, "RetryDelaySeconds", &config.storage.retryDelaySeconds);

            const json& commands = Section(parsed, "Commands", empty);
            Read(commands, "PlayerCommandsEnabled", &config.commands.playerCommandsEnabled);
            Read(commands, "CooldownSeconds", &config.commands.cooldownSeconds);
            Read(commands, "LastSeenLimit", &config.commands.lastSeenLimit);
        }
    }

    std::string messagesRaw;
    if (ReadWholeFile(messagesPath, &messagesRaw)) {
        json parsed = json::parse(StripJsonExtras(messagesRaw), nullptr, false);
        if (parsed.is_discarded()) {
            _failedFiles.push_back("Messages.json");
            if (_logger != nullptr) {
                _logger->Error("[Config] Messages.json разобрать не удалось — игроцкие "
                               "команды будут молчать. Файл НЕ перезаписан");
            }
        } else {
            const auto messages = parsed.find("Messages");
            if (messages != parsed.end() && messages->is_object()) {
                for (auto key = messages->begin(); key != messages->end(); ++key) {
                    if (!key.value().is_object()) continue;

                    std::map<std::string, std::string> byLanguage;
                    for (auto lang = key.value().begin(); lang != key.value().end(); ++lang) {
                        if (lang.value().is_string()) {
                            byLanguage[lang.key()] = lang.value().get<std::string>();
                        }
                    }
                    config.messages[key.key()] = byLanguage;
                }
            }
        }
    }

    // gamedata.json: создаётся, если нет; читается; НИКОГДА не перезаписывается —
    // это файл, который владелец правит после обновления игры
    const std::string gamedataPath = Join(configDirectory, "gamedata.json");
    if (!FileExists(gamedataPath) && !WriteWholeFile(gamedataPath, DefaultGamedataJson()) &&
        _logger != nullptr) {
        _logger->Error("[Config] не удалось записать " + gamedataPath);
    }

    std::string gamedataRaw;
    if (ReadWholeFile(gamedataPath, &gamedataRaw)) {
        json parsed = json::parse(StripJsonExtras(gamedataRaw), nullptr, false);
        if (parsed.is_discarded()) {
            _failedFiles.push_back("gamedata.json");
            if (_logger != nullptr) {
                _logger->Error("[Config] gamedata.json разобрать не удалось — итоги матча "
                               "останутся нулями. Файл НЕ перезаписан");
            }
        } else {
            const json empty = json::object();
            const json& entitySystem = Section(parsed, "GameEntitySystem", empty);
            Read(entitySystem, "linux", &config.gamedata.entitySystemOffsetLinux);
            Read(entitySystem, "windows", &config.gamedata.entitySystemOffsetWindows);
        }
    }

    Validate(config, configDirectory);
    return config;
}

}  // namespace ch
