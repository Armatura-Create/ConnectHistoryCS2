// Единица работы для фонового писателя.
//
// Через границу потоков проходят ТОЛЬКО эти структуры: ни одного указателя на
// сущность движка здесь нет и быть не может. Обращение к контроллеру из фонового
// потока — чтение чужой памяти и смерть процесса без стека в логе.
#pragma once

#include <cstdint>
#include <string>

namespace ch {

// Как закончилась сессия. Пишется в sessions.end_kind.
//
// Open существует потому, что строка сессии создаётся на ВХОДЕ игрока, а не
// на выходе: упавший сервер иначе не оставляет о сессии никаких следов вообще.
enum class SessionEndKind : uint8_t {
    // Сессия открыта и ещё не закрыта (в БД такого значения нет — там NULL в ended_at)
    Open = 0,
    // Обычный выход игрока (событие player_disconnect)
    Disconnect = 1,
    // Смена карты: движок отключает всех, но это не «уход» игрока
    MapChange = 2,
    // Штатная остановка сервера
    Shutdown = 3,
    // Выгрузка/перезагрузка плагина
    PluginUnload = 4,
    // Сессия найдена открытой при старте плагина — сервер до этого умер аварийно
    Stale = 5,
};

// Игровые итоги сессии. Снимаются только в главном потоке.
struct MatchStats {
    bool valid = false;

    int32_t kills = 0;
    int32_t deaths = 0;
    int32_t assists = 0;
    int32_t headShots = 0;
    int32_t damage = 0;
    int32_t mvps = 0;
    int32_t roundsPlayed = 0;

    // Финальная команда: 1 spectator, 2 T, 3 CT
    int32_t team = 0;
    int32_t teamChanges = 0;

    // score в этой цели не собирается: в CS2 нет игрового события, из которого
    // его можно взять, а чтение поля контроллера требует указателя на
    // CGameEntitySystem, добываемого захардкоженным смещением. Колонка остаётся
    // NULL — см. «расхождения между целями» в docs/DATABASE.md.
    bool hasScore = false;
    int32_t score = 0;
};

enum class JobKind : uint8_t {
    ServerUpsert = 0,
    SessionOpen = 1,
    SessionClose = 2,
    OnlineSnapshot = 3,
};

// Одна структура на все виды заданий вместо иерархии с полиморфной
// сериализацией: заданий четыре, они мелкие, и плоская запись переживает
// перезапуск процесса без единой строки кода про типы.
struct WriteJob {
    JobKind kind = JobKind::ServerUpsert;

    // Сколько раз задание уже пытались записать. Растёт при ретраях.
    int32_t attempts = 0;

    // --- общее
    int32_t serverId = 1;

    // --- ServerUpsert
    std::string address;
    std::string hostname;
    int64_t seenAt = 0;

    // --- SessionOpen / SessionClose
    // Ключ, сгенерированный плагином. Именно по нему идёт UPDATE: автоинкрементный
    // id из базы на момент выхода игрока может быть ещё не известен, потому что
    // запись идёт асинхронно.
    std::string sessionKey;
    uint64_t steamId64 = 0;
    uint32_t accountId = 0;
    std::string nickname;
    int64_t startedAt = 0;
    std::string connectMap;
    int32_t playersOnline = 0;
    int32_t maxPlayers = 0;

    bool hasClientLang = false;
    std::string clientLang;
    bool hasPlayerIp = false;
    std::string playerIp;
    bool hasIpHash = false;
    std::string ipHash;
    bool hasIpSubnet = false;
    std::string ipSubnet;
    bool hasCountryIso = false;
    std::string countryIso;
    bool hasCountryName = false;
    std::string countryName;
    bool hasCity = false;
    std::string city;

    std::string pluginVersion;

    // --- SessionClose
    int64_t endedAt = 0;
    int32_t durationSeconds = 0;

    // Сколько из них игрок провёл наблюдателем или без команды.
    int32_t spectatorSeconds = 0;

    // Учитывать ли наблюдательское время в «наиграно» (players.total_seconds).
    // Снимок настройки на момент закрытия: задание может пролежать в спуле,
    // и настройка за это время способна измениться — но строка обязана попасть
    // в базу по тем правилам, по которым была собрана.
    bool countSpectatorTime = true;

    std::string disconnectMap;
    int32_t disconnectReason = 0;
    std::string disconnectReasonName;
    SessionEndKind endKind = SessionEndKind::Disconnect;

    MatchStats stats;

    int32_t pingAvg = 0;
    int32_t pingMin = 0;
    int32_t pingMax = 0;
    int32_t pingSamples = 0;

    // --- OnlineSnapshot
    int64_t takenAt = 0;
    int32_t players = 0;
    int32_t bots = 0;
    std::string map;

    // Короткое описание для логов.
    std::string Describe() const;
};

}  // namespace ch
