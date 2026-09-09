// Плагин Metamod:Source.
//
// Здесь и только здесь живут обращения к движку. Всё остальное — в src/core,
// которое про SDK ничего не знает и потому проверяется тестами на любой машине.
//
// Осознанное ограничение: В ЭТОМ ПЛАГИНЕ нет ни сигнатур, ни смещений, ни
// резолва vtable по RTTI. Всё, что он делает сам, идёт через хуки Metamod и
// интерфейсы движка, полученные фабрикой, — и потому не ломается на очередном
// обновлении игры.
//
// Итоги матча — единственное, чему это правило мешало: счёт и статистика
// живут в полях контроллера, а до контроллера нужен указатель на CEntitySystem,
// которого ни одна фабрика не отдаёт. Он лежит внутри IGameResourceService по
// смещению — и это ЕДИНСТВЕННОЕ число в плагине, зависящее от версии игры.
// Живёт оно в gamedata.json, правится без пересборки (см. mm/stats.cpp).
// Сами поля читаются по именам через ISchemaSystem — это схема игры, не gamedata.
// Игровые события не нужны: смена команды ловится опросом контроллера раз
// в секунду, раунды — по счётчику gamerules.
//
// История подключений — то, ради чего плагин существует, — ни от чего из этого
// не зависит: вход, выход, время, карта, страна, пинг и «кто сейчас онлайн»
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
// Полные типы нужны KHook::Virtual: индекс в vtable считается из указателя
// на метод, а для этого класс должен быть определён, не объявлен.
#include <eiface.h>
#include <icvar.h>
#include <memory>
// uint64 SDK - это unsigned long long, а uint64_t на Linux - unsigned long.
// Разные типы: сигнатуры хуков обязаны повторять SDK дословно, иначе делегат
// SourceHook не совпадёт с объявлением интерфейса.
#include <tier0/platform.h>
#include <string>
#include <unordered_map>


// g_SMAPI, g_PLAPI, g_PLID и указатель KHook. Объявить их обязан КАЖДЫЙ файл
// цели: META_CONPRINTF и хуки KHook — макросы и шаблоны поверх этих указателей,
// а определяет их PLUGIN_EXPOSE ровно один раз, в самом низу plugin.cpp.
PLUGIN_GLOBALVARS();

namespace ch {

class ConsoleLogger;

class ConnectHistoryPlugin final : public ISmmPlugin, public IMetamodListener {
public:
    // Хуки KHook привязываются к методам в конструкторе: индекс vtable
    // вычисляется из указателя на член и не трогает движок, поэтому
    // это безопасно даже для глобального объекта со статической инициализацией.
    // К самому движку хуки цепляются в Load (Add) и отцепляются в Unload (Remove).
    ConnectHistoryPlugin();

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

    // Хуки движка (KHook). Первый аргумент — объект, чей метод перехвачен;
    // нам он не нужен, но так устроена сигнатура колбэка.
    KHook::Return<void> Hook_OnClientConnected(IServerGameClients*, CPlayerSlot slot,
                                               const char* name, uint64 xuid,
                                               const char* networkId, const char* address,
                                               bool fake);
    KHook::Return<void> Hook_ClientPutInServer(IServerGameClients*, CPlayerSlot slot,
                                               char const* name, int type, uint64 xuid);
    KHook::Return<void> Hook_ClientDisconnect(IServerGameClients*, CPlayerSlot slot,
                                              ENetworkDisconnectionReason reason,
                                              const char* name, uint64 xuid,
                                              const char* networkId);
    KHook::Return<void> Hook_GameFrame(IServerGameDLL*, bool simulating, bool firstTick,
                                       bool lastTick);

    // Чат игрока. Через ICvar проходят и say, и консольные команды клиента —
    // это единственная точка, где чат слышен без игровых событий.
    KHook::Return<void> Hook_DispatchConCommand(ICvar*, ConCommandRef command,
                                                const CCommandContext& context,
                                                const CCommand& args);


    // Команды (см. commands.cpp)
    void CommandStatus();
    void CommandReload();
    // SteamID игрока в слоте. 0 — слот пуст, бот или игрок уже вышел.
    uint64_t SteamIdForSlot(int slot) const;

    // slot — кому отвечать. -1 означает «ответ в консоль сервера»: так работает
    // админский вызов про офлайн-игрока.
    void CommandPlaytime(uint64_t steamId, int slot);
    void CommandLastSeen(uint64_t steamId, int slot);

    const Config& GetConfig() const { return _config; }

private:
    void RegisterPluginCommands();
    void UnregisterPluginCommands();

    void BuildDatabaseStack();
    void ReloadConfig();
    void RegisterServer();
    void TakeOnlineSnapshot();
    void OpenSessionFor(int slot, uint64_t steamId, const char* name);
    // slot — где сейчас сидит игрок, чтобы снять итоги с контроллера;
    // -1, если слота уже нет (сессия закрывается не из хука отключения).
    void CloseSession(OpenSession session, SessionEndKind kind, int reason,
                      const std::string& reasonName, int slot = -1);
    int SlotForSteamId(uint64_t steamId) const;
    void CloseAllSessions(SessionEndKind kind);
    std::string CurrentMap() const;
    std::string ReadConVar(const char* name) const;
    int CountHumans() const;
    int MaxPlayers() const;
    void SendChat(int slot, const std::string& message);
    std::string Localize(const std::string& key, const std::string& lang) const;

    // Язык клиента из его же cl_language. Пусто -> DefaultLang из конфига.
    std::string ClientLanguage(int slot) const;

    // Разбирает "!playtime" / "/lastseen" из чата. true — команда наша
    // и в общий чат уходить не должна.
    bool HandleChatCommand(int slot, const char* text);

    void SamplePings();

    // Раз в секунду: смена команды у каждой сессии и счётчик раундов gamerules.
    // Заменяет события player_team и round_end, до которых без сигнатур
    // не добраться. Секунда — точность spectator_seconds; дороже не нужно.
    void PollMatchState(int64_t now);

    // Что известно про слот между OnClientConnected и ClientPutInServer.
    // Ключуется по слоту сознательно и живёт ровно до открытия сессии: сама
    // сессия хранится по SteamID, потому что слот движок переиспользует.
    struct ClientSlot {
        uint64_t steamId = 0;
        std::string ip;
        bool fake = false;
    };

    KHook::Virtual<IServerGameDLL, void, bool, bool, bool> _hookGameFrame;
    KHook::Virtual<IServerGameClients, void, CPlayerSlot, const char*, uint64, const char*,
                   const char*, bool>
        _hookOnClientConnected;
    KHook::Virtual<IServerGameClients, void, CPlayerSlot, char const*, int, uint64>
        _hookClientPutInServer;
    KHook::Virtual<IServerGameClients, void, CPlayerSlot, ENetworkDisconnectionReason,
                   const char*, uint64, const char*>
        _hookClientDisconnect;
    KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext&, const CCommand&>
        _hookDispatchConCommand;

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
    int64_t _nextPingAt = 0;
    int64_t _nextPollAt = 0;
    // Последний виденный m_totalRoundsPlayed; -1 — карта ещё не читалась
    int32_t _lastRoundsPlayed = -1;
    int64_t _registerServerAt = 0;
    bool _serverRegistered = false;
    bool _loaded = false;
};

extern ConnectHistoryPlugin g_plugin;

}  // namespace ch
