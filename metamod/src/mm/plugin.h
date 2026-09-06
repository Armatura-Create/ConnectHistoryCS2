// Плагин Metamod:Source.
//
// Здесь и только здесь живут обращения к движку. Всё остальное — в src/core,
// которое про SDK ничего не знает и потому проверяется тестами на любой машине.
//
// Осознанное ограничение: НИКАКИХ сигнатур, смещений и детуров. Всё нужное
// доступно через хуки Metamod, игровые события и интерфейсы движка. Цена —
// колонки score и ping_* остаются NULL (см. «расхождения между целями»
// в docs/DATABASE.md), выгода — плагин не ломается на очередном обновлении игры.
//
// Почему именно эти две. Счёт и пинг живут только в полях контроллера, а чтобы
// добраться до контроллера, нужен указатель на CGameEntitySystem, добываемый
// смещением от GameResourceServiceServer. Это единственная константа, которую
// пришлось бы захардкодить, и ломается она ровно тогда, когда Valve двигает
// структуру, — то есть в любое обновление. Убийства, смерти, помощь, урон,
// MVP и раунды берутся из игровых событий и такой цены не требуют.
#pragma once

#include "core/config.h"
#include "core/geoip.h"
#include "core/query_service.h"
#include "core/session.h"
#include "core/writer.h"
#include "db/mariadb.h"

#include <ISmmPlugin.h>
#include <igameevents.h>
#include <memory>
#include <string>
#include <unordered_map>

class CPlayerSlot;
enum ENetworkDisconnectionReason : int;

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
    void Hook_OnClientConnected(CPlayerSlot slot, const char* name, uint64_t xuid,
                                const char* networkId, const char* address, bool fake);
    void Hook_ClientPutInServer(CPlayerSlot slot, const char* name, int type, uint64_t xuid);
    void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
                               const char* name, uint64_t xuid, const char* networkId);
    void Hook_GameFrame(bool simulating, bool firstTick, bool lastTick);
    int Hook_LoadEventsFromFile(const char* fileName, bool searchAll);

    // Игровые события (см. events.cpp)
    void OnRoundEnd();
    void OnPlayerTeam(uint64_t steamId, int team);
    void OnPlayerDeath(uint64_t attacker, uint64_t victim, uint64_t assister, bool headshot);
    void OnPlayerHurt(uint64_t attacker, int damageHealth);
    void OnRoundMvp(uint64_t steamId);

    // Команды (см. commands.cpp)
    void CommandStatus();
    void CommandReload();
    void CommandPlaytime(uint64_t steamId);
    void CommandLastSeen(uint64_t steamId);

    const Config& GetConfig() const { return _config; }

private:
    void RegisterEventListeners();
    void UnregisterEventListeners();
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
    struct PendingClient {
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
    std::unordered_map<int, PendingClient> _pending;
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
