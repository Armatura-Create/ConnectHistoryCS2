// Плагин Metamod:Source.
//
// Здесь и только здесь живут обращения к движку. Всё остальное — в src/core,
// которое про SDK ничего не знает и потому проверяется тестами на любой машине.
//
// Осознанное ограничение: НИКАКИХ сигнатур, смещений и резолва vtable по RTTI.
// Всё нужное доступно через хуки Metamod и интерфейсы движка, полученные
// фабрикой. Цена — итоги матча в этой цели не заполняются (см. «расхождения
// между целями» в docs/DATABASE.md), выгода — плагин не ломается на очередном
// обновлении игры.
//
// Почему цена именно такая. Счёт и пинг живут в полях контроллера, а до
// контроллера нужен указатель на CGameEntitySystem, добываемый смещением от
// GameResourceServiceServer. Убийства, смерти, помощь, урон, MVP, раунды и
// смена команды приходят игровыми событиями, но IGameEventManager2 в CS2
// не отдаётся ни одной фабрикой: единственный путь к нему — найти vtable
// класса CGameEventManager по имени в символах server.so или по RTTI
// в server.dll. И то, и другое — чтение чужой памяти по угаданному адресу,
// которое нечем проверить в CI и которое роняет сервер, если ошиблось.
// История подключений — то, ради чего плагин существует, — от этого
// не зависит: вход, выход, время, карта, страна и «кто сейчас онлайн»
// берутся из хуков и работают без единого смещения.
#pragma once

#include "core/config.h"
#include "core/geoip.h"
#include "core/query_service.h"
#include "core/session.h"
#include "core/writer.h"
#include "db/mariadb.h"

#include <cstdint>
#include <ISmmPlugin.h>
#include <memory>
// uint64 SDK - это unsigned long long, а uint64_t на Linux - unsigned long.
// Разные типы: сигнатуры хуков обязаны повторять SDK дословно, иначе делегат
// SourceHook не совпадёт с объявлением интерфейса.
#include <tier0/platform.h>
#include <string>
#include <unordered_map>

class CPlayerSlot;
enum ENetworkDisconnectionReason : int;

// g_SMAPI, g_PLAPI, g_PLID, g_SHPtr. Объявить их обязан КАЖДЫЙ файл цели:
// META_CONPRINTF и SH_ADD_HOOK — это макросы поверх этих указателей, а
// определяет их PLUGIN_EXPOSE ровно один раз, в самом низу plugin.cpp.
PLUGIN_GLOBALVARS();

namespace ch {

class ConsoleLogger;

class ConnectHistoryPlugin final : public ISmmPlugin, public IMetamodListener {
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    bool Unload(char* error, size_t maxlen) override;

    const char* GetAuthor() override { return "Armatura"; }
    const char* GetName() override { return "ConnectHistory"; }
    const char* GetDescription() override {
        return "История подключений и статистика игроков в MySQL";
    }
    const char* GetURL() override { return "https://github.com/Armatura-Create/ConnectHistoryCS2"; }
    const char* GetLicense() override { return "GPL-3.0-or-later"; }
    const char* GetVersion() override;
    const char* GetDate() override { return __DATE__; }
    const char* GetLogTag() override { return "ConnectHistory"; }

    // IMetamodListener: смена карты — граница сессии
    void OnLevelInit(const char* mapName, const char* mapEntities, const char* oldLevel,
                     const char* landmarkName, bool loadGame, bool background) override;
    void OnLevelShutdown() override;

    // Хуки Metamod
    void Hook_OnClientConnected(CPlayerSlot slot, const char* name, uint64 xuid,
                                const char* networkId, const char* address, bool fake);
    void Hook_ClientPutInServer(CPlayerSlot slot, char const* name, int type, uint64 xuid);
    void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
                               const char* name, uint64 xuid, const char* networkId);
    void Hook_GameFrame(bool simulating, bool firstTick, bool lastTick);

    // Команды (см. commands.cpp)
    void CommandStatus();
    void CommandReload();
    void CommandPlaytime(uint64_t steamId);
    void CommandLastSeen(uint64_t steamId);

    const Config& GetConfig() const { return _config; }

private:
    void RegisterPluginCommands();
    void UnregisterPluginCommands();

    void BuildDatabaseStack();
    void ReloadConfig();
    void RegisterServer();
    void TakeOnlineSnapshot();
    void OpenSessionFor(int slot, uint64_t steamId, const char* name);
    void CloseSession(OpenSession session, SessionEndKind kind, int reason,
                      const std::string& reasonName);
    void CloseAllSessions(SessionEndKind kind);
    std::string CurrentMap() const;
    std::string ReadConVar(const char* name) const;
    int CountHumans() const;
    int MaxPlayers() const;
    void SendChat(uint64_t steamId, const std::string& message);
    std::string Localize(const std::string& key, const std::string& lang) const;

    // Что известно про слот между OnClientConnected и ClientPutInServer.
    // Ключуется по слоту сознательно и живёт ровно до открытия сессии: сама
    // сессия хранится по SteamID, потому что слот движок переиспользует.
    struct ClientSlot {
        uint64_t steamId = 0;
        std::string ip;
        bool fake = false;
    };

    Config _config;
    std::unique_ptr<ConsoleLogger> _logger;
    std::unique_ptr<MariaDatabase> _database;
    std::unique_ptr<SessionWriter> _writer;
    std::unique_ptr<QueryService> _query;
    std::unique_ptr<GeoIpService> _geoIp;

    SessionRegistry _sessions;
    std::unordered_map<int, ClientSlot> _slots;
    std::unordered_map<uint64_t, int64_t> _commandCooldown;

    std::string _configDirectory;
    std::string _dataDirectory;
    std::string _pluginDirectory;
    std::string _version;

    int32_t _displayOffsetSeconds = 0;

    int64_t _nextSnapshotAt = 0;
    int64_t _registerServerAt = 0;
    bool _serverRegistered = false;
    bool _loaded = false;
};

extern ConnectHistoryPlugin g_plugin;

}  // namespace ch
