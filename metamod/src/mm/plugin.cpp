#include "mm/plugin.h"

#include "core/banner.h"
#include "core/logger.h"
#include "core/schema_service.h"
#include "core/util/chat_format.h"
#include "core/util/ip.h"
#include "core/util/sql_sanitizer.h"
#include "core/util/steamid.h"
#include "core/util/timeutil.h"
#include "mm/globals.h"
#include "mm/version.h"

#include <eiface.h>
#include <icvar.h>
#include <iserver.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

// Хуки Metamod. Сигнатуры сверены с CS2Fixes — единственным широко
// используемым MM:S-плагином под CS2; при расхождении сборка упадёт в CI,
// а не молча на боевом сервере.
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot,
                   char const*, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot,
                   ENetworkDisconnectionReason, const char*, uint64, const char*);
SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0, CPlayerSlot,
                   const char*, uint64, const char*, const char*, bool);

namespace ch {

IVEngineServer2* g_engine = nullptr;
ISource2Server* g_server = nullptr;
IServerGameClients* g_gameClients = nullptr;

ConnectHistoryPlugin g_plugin;

// Логгер в консоль сервера. Debug печатает SteamID, ники и IP игроков —
// по умолчанию выключен.
class ConsoleLogger final : public ILogger {
public:
    explicit ConsoleLogger(const Config* config) : _config(config) {}

    void Info(const std::string& message) override { Write("INFO", message); }
    void Warn(const std::string& message) override { Write("WARN", message); }
    void Error(const std::string& message) override { Write("ERROR", MaskSecrets(message)); }

    void Debug(const std::string& message) override {
        if (_config != nullptr && _config->debug) Write("DEBUG", message);
    }

    void Raw(const std::string& message) override {
        META_CONPRINTF("%s\n", message.c_str());
    }

private:
    static void Write(const char* level, const std::string& message) {
        META_CONPRINTF("[ConnectHistory] [%s] %s\n", level, message.c_str());
    }

    const Config* _config;
};

namespace {

// Ключ сессии — GUID, а не автоинкремент из базы: запись идёт асинхронно,
// и на момент выхода игрока id может быть ещё не известен.
std::string NewSessionKey() {
    static std::mt19937_64 generator(
        static_cast<uint64_t>(UtcNowSeconds()) ^ 0x9E3779B97F4A7C15ull);

    static const char* const kDigits = "0123456789abcdef";
    std::string key;
    key.reserve(32);
    for (int i = 0; i < 4; ++i) {
        const uint64_t chunk = generator();
        for (int shift = 60; shift >= 0; shift -= 4) {
            key.push_back(kDigits[(chunk >> shift) & 0xF]);
            if (key.size() == 32) break;
        }
    }
    return key;
}

// Ник — недоверенные данные: длину задаёт игрок, а колонка в базе конечна.
// Режем по границе UTF-8: обрезанный посередине символ превращает валидный
// utf8mb4 в мусор, который MySQL отвергнет целиком вместе с сессией.
std::string TruncateUtf8(const char* value, size_t maxBytes) {
    if (value == nullptr) return std::string();

    std::string text(value);
    if (text.size() <= maxBytes) return text;

    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

std::string ReasonName(int reason) {
    // Имя причины движок наружу не отдаёт, а таблица из полутора сотен констант
    // разъедется с игрой на первом же обновлении. Код не теряем — он остаётся
    // в disconnect_reason; имя строим из него же.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "REASON_%d", reason);
    return std::string(buffer);
}

}  // namespace

const char* ConnectHistoryPlugin::GetVersion() { return CH_VERSION; }

bool ConnectHistoryPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen,
                                bool late) {
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_engine, IVEngineServer2,
                        SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_server, ISource2Server,
                    SOURCE2SERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_gameClients, IServerGameClients,
                    SOURCE2GAMECLIENTS_INTERFACE_VERSION);

    // Нужно, чтобы приходили события IMetamodListener (в том числе OnLevelInit)
    g_SMAPI->AddListener(this, this);

    const char* baseDirectory = g_SMAPI->GetBaseDir();
    _pluginDirectory = std::string(baseDirectory) + "/addons/ConnectHistory";
    _dataDirectory = _pluginDirectory + "/data";

    // Логгер нужен раньше конфига: выбор каталога уже может о чём-то сообщить.
    if (!_logger) _logger.reset(new ConsoleLogger(&_config));
    _configDirectory = ChooseConfigDirectory(baseDirectory, _logger.get());
    _version = CH_VERSION;

    ReloadConfig();

    // Заставка печатается ПОСЛЕ загрузки конфига: в ней номер сервера,
    // а он приходит оттуда.
    for (const std::string& line : BuildBanner(_version, "Metamod:Source", _config.serverId)) {
        _logger->Raw(line);
    }

    _geoIp.reset(new GeoIpService(_pluginDirectory, _logger.get()));

    BuildDatabaseStack();

    SH_ADD_HOOK(IServerGameDLL, GameFrame, g_server,
                SH_MEMBER(this, &ConnectHistoryPlugin::Hook_GameFrame), true);
    SH_ADD_HOOK(IServerGameClients, OnClientConnected, g_gameClients,
                SH_MEMBER(this, &ConnectHistoryPlugin::Hook_OnClientConnected), false);
    SH_ADD_HOOK(IServerGameClients, ClientPutInServer, g_gameClients,
                SH_MEMBER(this, &ConnectHistoryPlugin::Hook_ClientPutInServer), true);
    SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_gameClients,
                SH_MEMBER(this, &ConnectHistoryPlugin::Hook_ClientDisconnect), true);

    RegisterPluginCommands();

    // Регистрацию сервера откладываем: на этом этапе движок ещё не обязан
    // отдать ConVar, а ошибка здесь стоит загрузки плагина.
    _registerServerAt = UtcNowSeconds() + 3;
    _nextSnapshotAt =
        UtcNowSeconds() + std::max(30, _config.collect.onlineSnapshotIntervalSeconds);

    _loaded = true;

    if (late) {
        // Плагин загрузили на живой сервер: у игроков уже идут сессии, о которых
        // мы ничего не знаем. Открыть их без данных о подключении нельзя —
        // они появятся при следующем заходе.
        _logger->Warn("[Plugin] Поздняя загрузка: сессии уже подключённых игроков "
                      "начнутся с их следующего входа");
    }

    (void)id;
    (void)error;
    (void)maxlen;
    return true;
}

bool ConnectHistoryPlugin::Unload(char* error, size_t maxlen) {
    (void)error;
    (void)maxlen;

    UnregisterPluginCommands();

    SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_server,
                   SH_MEMBER(this, &ConnectHistoryPlugin::Hook_GameFrame), true);
    SH_REMOVE_HOOK(IServerGameClients, OnClientConnected, g_gameClients,
                   SH_MEMBER(this, &ConnectHistoryPlugin::Hook_OnClientConnected), false);
    SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, g_gameClients,
                   SH_MEMBER(this, &ConnectHistoryPlugin::Hook_ClientPutInServer), true);
    SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_gameClients,
                   SH_MEMBER(this, &ConnectHistoryPlugin::Hook_ClientDisconnect), true);

    // Открытые сессии закрываем явно: иначе выгрузка плагина оставляет их
    // висеть в базе, и «кто сейчас онлайн» врёт до следующего старта.
    CloseAllSessions(SessionEndKind::PluginUnload);

    // Писателя останавливаем ДО закрытия соединения: остаток очереди должен
    // успеть уйти в базу, а что не успеет — в спул.
    _writer.reset();
    _query.reset();
    _database.reset();
    _geoIp.reset();

    _loaded = false;
    return true;
}

void ConnectHistoryPlugin::ReloadConfig() {
    // Логгер создаётся первым и держит указатель на _config: флаг Debug
    // подхватывается сразу после перечитывания конфига.
    if (!_logger) _logger.reset(new ConsoleLogger(&_config));

    ConfigService service(_logger.get());
    _config = service.LoadOrCreate(_configDirectory);
    _displayOffsetSeconds =
        ResolveDisplayOffsetSeconds(_config.displayTimeZone, _logger.get());
}

void ConnectHistoryPlugin::BuildDatabaseStack() {
    _database.reset(new MariaDatabase(_config.database, _logger.get()));
    _database->LogTarget();

    _writer.reset(
        new SessionWriter(_database.get(), _config.storage, _dataDirectory, _logger.get()));
    _query.reset(new QueryService(_database.get(), _logger.get()));

    // Схема создаётся синхронно: в отличие от C#-целей здесь нет фонового
    // планировщика, а Load() плагина Metamod не держит загрузку карты.
    SchemaService schema(_database.get(), _logger.get());
    if (schema.EnsureSchema()) {
        schema.MarkStaleSessions(_config.serverId);
    }
}

void ConnectHistoryPlugin::OnLevelInit(const char* mapName, const char* mapEntities,
                                       const char* oldLevel, const char* landmarkName,
                                       bool loadGame, bool background) {
    (void)mapName;
    (void)mapEntities;
    (void)oldLevel;
    (void)landmarkName;
    (void)loadGame;
    (void)background;

    // Смена карты — граница сессии: так в данных остаётся, СКОЛЬКО игрок провёл
    // на конкретной карте, а не размазанное по нескольким картам время.
    CloseAllSessions(SessionEndKind::MapChange);
    RegisterServer();
}

void ConnectHistoryPlugin::OnLevelShutdown() {}

void ConnectHistoryPlugin::Hook_OnClientConnected(CPlayerSlot slot, const char* name,
                                                  uint64 xuid, const char* networkId,
                                                  const char* address, bool fake) {
    (void)name;
    (void)networkId;

    // Единственное место, где движок отдаёт IP игрока. Запоминаем до
    // ClientPutInServer — сессию открываем там, когда игрок реально в игре.
    ClientSlot entry;
    entry.steamId = xuid;
    entry.ip = address != nullptr ? ip::ExtractIp(address) : std::string();
    entry.fake = fake;

    _slots[slot.Get()] = entry;

    RETURN_META(MRES_IGNORED);
}

void ConnectHistoryPlugin::Hook_ClientPutInServer(CPlayerSlot slot, char const* name,
                                                  int type, uint64 xuid) {
    (void)type;

    OpenSessionFor(slot.Get(), xuid, name);
    RETURN_META(MRES_IGNORED);
}

void ConnectHistoryPlugin::Hook_ClientDisconnect(CPlayerSlot slot,
                                                 ENetworkDisconnectionReason reason,
                                                 const char* name, uint64 xuid,
                                                 const char* networkId) {
    (void)name;
    (void)networkId;

    _slots.erase(slot.Get());

    OpenSession session;
    if (_sessions.Take(xuid, &session)) {
        CloseSession(std::move(session), SessionEndKind::Disconnect,
                     static_cast<int>(reason), ReasonName(static_cast<int>(reason)));
    }

    // Словарь кулдаунов чистим здесь — иначе он растёт всё время жизни сервера
    _commandCooldown.erase(xuid);

    RETURN_META(MRES_IGNORED);
}

// Периодическая работа. Кадр — единственное место, где мы гарантированно
// в главном потоке и можем трогать движок.
void ConnectHistoryPlugin::Hook_GameFrame(bool simulating, bool firstTick, bool lastTick) {
    (void)simulating;
    (void)firstTick;
    (void)lastTick;

    if (!_loaded) RETURN_META(MRES_IGNORED);

    const int64_t now = UtcNowSeconds();

    if (!_serverRegistered && _registerServerAt != 0 && now >= _registerServerAt) {
        _serverRegistered = true;
        RegisterServer();
    }

    if (_config.collect.onlineSnapshots && now >= _nextSnapshotAt) {
        _nextSnapshotAt = now + std::max(30, _config.collect.onlineSnapshotIntervalSeconds);
        TakeOnlineSnapshot();
    }

    RETURN_META(MRES_IGNORED);
}

void ConnectHistoryPlugin::OpenSessionFor(int slot, uint64_t steamId, const char* name) {
    if (!IsRealSteamId(steamId)) return;

    const auto known = _slots.find(slot);
    const bool fake = known != _slots.end() && known->second.fake;
    if (fake) return;

    const std::string ip = known != _slots.end() ? known->second.ip : std::string();
    const std::string nickname = TruncateUtf8(name, 128);

    // Повторный вход без смены карты (реконнект) не должен плодить открытые
    // строки: старую закрываем как смену карты.
    OpenSession previous;
    if (_sessions.Take(steamId, &previous)) {
        CloseSession(std::move(previous), SessionEndKind::MapChange, 0, "MapChange");
    }

    GeoInfo geo;
    if (_config.collect.geoIp && _geoIp) geo = _geoIp->Lookup(ip);

    OpenSession session;
    session.key = NewSessionKey();
    session.steamId64 = steamId;
    session.accountId = ToAccountId(steamId);
    session.startedAt = UtcNowSeconds();
    session.connectMap = CurrentMap();
    session.nickname = nickname;
    session.countryIso = geo.iso;

    // Отсчёт времени вне игры начинается вместе с сессией: до первого
    // player_team игрок выбирает команду, и это время тоже не игровое.
    session.StartTeamTracking(session.startedAt);

    WriteJob job;
    job.kind = JobKind::SessionOpen;
    job.serverId = _config.serverId;
    job.sessionKey = session.key;
    job.steamId64 = session.steamId64;
    job.accountId = session.accountId;
    job.nickname = session.nickname;
    job.startedAt = session.startedAt;
    job.connectMap = session.connectMap;
    job.playersOnline = CountHumans();
    job.maxPlayers = MaxPlayers();
    job.pluginVersion = _version;

    if (_config.collect.playerIp && !ip.empty()) {
        job.hasPlayerIp = true;
        job.playerIp = ip;
    }
    if (_config.collect.ipHash) {
        const std::string hash = ip::Hash(ip, _config.collect.ipHashSalt);
        if (!hash.empty()) {
            job.hasIpHash = true;
            job.ipHash = hash;
        }
    }
    if (_config.collect.playerIp || _config.collect.ipHash) {
        const std::string subnet = ip::ToSubnet(ip);
        if (!subnet.empty()) {
            job.hasIpSubnet = true;
            job.ipSubnet = subnet;
        }
    }

    job.hasCountryIso = geo.hasIso;
    job.countryIso = geo.iso;
    job.hasCountryName = geo.hasCountry;
    job.countryName = geo.country;
    job.hasCity = geo.hasCity;
    job.city = geo.city;

    _sessions.Add(session);
    if (_writer) _writer->Enqueue(job);

    _logger->Debug("[JOIN] сессия " + session.key + " открыта: " + nickname + " (" +
                   std::to_string(steamId) + "), карта " + session.connectMap);
}

void ConnectHistoryPlugin::CloseSession(OpenSession session, SessionEndKind kind,
                                        int reason, const std::string& reasonName) {
    const int64_t endedAt = UtcNowSeconds();
    const int duration =
        static_cast<int>(std::max<int64_t>(0, endedAt - session.startedAt));

    // Последний интервал команды закрывается здесь, иначе время после
    // последней смены команды нигде не учтётся.
    session.FinishTeamTracking(endedAt);

    // Наблюдательское время не может превышать саму сессию: часы сервера могут
    // прыгнуть, а отрицательное «наиграно» испортит агрегат навсегда.
    const int spectator = std::min(std::max(0, session.SpectatorSeconds()), duration);

    WriteJob job;
    job.kind = JobKind::SessionClose;
    job.serverId = _config.serverId;
    job.sessionKey = session.key;
    job.steamId64 = session.steamId64;
    job.accountId = session.accountId;
    job.nickname = session.nickname;
    job.startedAt = session.startedAt;
    job.endedAt = endedAt;
    job.durationSeconds = duration;
    job.spectatorSeconds = spectator;
    job.countSpectatorTime = _config.collect.countSpectatorTime;
    job.disconnectMap = CurrentMap();
    job.disconnectReason = reason;
    job.disconnectReasonName = reasonName;
    job.endKind = kind;

    if (_config.collect.matchStats) {
        job.stats = session.Stats();
        job.stats.valid = true;
        job.stats.roundsPlayed = session.RoundsPlayed();
        job.stats.teamChanges = session.TeamChanges();
        job.stats.team = session.LastTeam() < 0 ? 0 : session.LastTeam();
    }

    // ping_* в этой цели не собираются — см. комментарий к классу
    job.pingSamples = 0;

    if (_writer) _writer->Enqueue(job);

    _logger->Debug("[LEAVE] сессия " + session.key + " закрыта: " + session.nickname + ", " +
                   std::to_string(duration) + " с, причина " + reasonName);
}

void ConnectHistoryPlugin::CloseAllSessions(SessionEndKind kind) {
    const char* name = kind == SessionEndKind::MapChange ? "MapChange" : "PluginUnload";
    for (OpenSession& session : _sessions.TakeAll()) {
        CloseSession(std::move(session), kind, 0, name);
    }
}

void ConnectHistoryPlugin::RegisterServer() {
    if (!_writer) return;

    WriteJob job;
    job.kind = JobKind::ServerUpsert;
    job.serverId = _config.serverId;
    job.hostname = ReadConVar("hostname");
    job.seenAt = UtcNowSeconds();

    // Публичный адрес берётся из конфига: процесс игрового сервера своего
    // внешнего адреса не знает, а ConVar ip — это адрес привязки сокета,
    // при обычной настройке 0.0.0.0.
    const std::string bindIp = ReadConVar("ip");
    int port = 0;
    const std::string hostport = ReadConVar("hostport");
    if (!hostport.empty()) port = std::atoi(hostport.c_str());

    job.address = ip::ResolvePublicAddress(_config.server.publicAddress, bindIp, port);

    if (job.address.empty()) {
        _logger->Warn(_config.server.publicAddress.empty()
                          ? "[Plugin] Публичный адрес сервера определить не удалось "
                            "(ConVar ip — это адрес привязки сокета). Пропишите "
                            "Server.PublicAddress в Settings.json"
                          : "[Plugin] Server.PublicAddress не похож на публичный адрес. "
                            "Колонка address останется пустой");
    }

    _writer->Enqueue(job);
}

void ConnectHistoryPlugin::TakeOnlineSnapshot() {
    if (!_writer) return;

    WriteJob job;
    job.kind = JobKind::OnlineSnapshot;
    job.serverId = _config.serverId;
    job.takenAt = UtcNowSeconds();
    job.players = CountHumans();
    job.bots = 0;
    job.maxPlayers = MaxPlayers();
    job.map = CurrentMap();

    _writer->Enqueue(job);
}

std::string ConnectHistoryPlugin::CurrentMap() const {
    if (g_engine == nullptr) return std::string();

    const CGlobalVars* globals = g_engine->GetServerGlobals();
    if (globals == nullptr) return std::string();

    // ToCStr() у string_t сам отдаёт "" вместо nullptr, проверять отдельно нечего
    return std::string(globals->mapname.ToCStr());
}

std::string ConnectHistoryPlugin::ReadConVar(const char* name) const {
    if (g_pCVar == nullptr) return std::string();

    ConVarRefAbstract convar(name);
    if (!convar.IsValidRef()) return std::string();

    const CUtlString value = convar.GetString();
    const char* text = value.Get();
    return text != nullptr ? std::string(text) : std::string();
}

int ConnectHistoryPlugin::CountHumans() const {
    // Сессии — единственный источник, который у нас точно верен: он ведётся
    // по SteamID и не зависит от того, как движок нумерует слоты.
    return static_cast<int>(_sessions.Count());
}

int ConnectHistoryPlugin::MaxPlayers() const {
    if (g_engine == nullptr) return 0;

    const CGlobalVars* globals = g_engine->GetServerGlobals();
    return globals == nullptr ? 0 : globals->maxClients;
}

}  // namespace ch

PLUGIN_EXPOSE(ConnectHistoryPlugin, ch::g_plugin);
