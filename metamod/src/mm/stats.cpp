#include "mm/stats.h"

#include "core/jobs.h"
#include "mm/globals.h"
#include "mm/plugin.h"
#include "mm/schema.h"

#include <entity2/entityidentity.h>
#include <entity2/entitysystem.h>

#include <cstdint>
#include <cstring>

namespace ch {
namespace stats {
namespace {

// Все смещения спрашиваются один раз и живут до выгрузки: схема — часть игры,
// в пределах одного запуска сервера она не меняется.
struct Offsets {
    int32_t teamNum = -1;
    int32_t score = -1;
    int32_t mvps = -1;
    int32_t actionTracking = -1;
    int32_t matchStats = -1;
    int32_t kills = -1;
    int32_t deaths = -1;
    int32_t assists = -1;
    int32_t headShots = -1;
    int32_t damage = -1;
    int32_t gameRules = -1;
    int32_t roundsPlayed = -1;

    bool ControllerComplete() const {
        return teamNum >= 0 && score >= 0 && mvps >= 0 && actionTracking >= 0 &&
               matchStats >= 0 && kills >= 0 && deaths >= 0 && assists >= 0 &&
               headShots >= 0 && damage >= 0;
    }
};

const Offsets& Resolve() {
    static Offsets offsets;
    static bool resolved = false;
    if (resolved) return offsets;
    resolved = true;

    // Имена классов и полей — те же, к которым обращается C#-цель через
    // CounterStrikeSharp; там они тоже берутся из схемы игры.
    offsets.teamNum = schema::FieldOffset("CCSPlayerController", "m_iTeamNum");
    offsets.score = schema::FieldOffset("CCSPlayerController", "m_iScore");
    offsets.mvps = schema::FieldOffset("CCSPlayerController", "m_iMVPs");
    offsets.actionTracking =
        schema::FieldOffset("CCSPlayerController", "m_pActionTrackingServices");
    offsets.matchStats =
        schema::FieldOffset("CCSPlayerController_ActionTrackingServices", "m_matchStats");
    offsets.kills = schema::FieldOffset("CSMatchStats_t", "m_iKills");
    offsets.deaths = schema::FieldOffset("CSMatchStats_t", "m_iDeaths");
    offsets.assists = schema::FieldOffset("CSMatchStats_t", "m_iAssists");
    offsets.headShots = schema::FieldOffset("CSMatchStats_t", "m_iHeadShotKills");
    offsets.damage = schema::FieldOffset("CSMatchStats_t", "m_iDamage");
    offsets.gameRules = schema::FieldOffset("CCSGameRulesProxy", "m_pGameRules");
    offsets.roundsPlayed = schema::FieldOffset("CCSGameRules", "m_totalRoundsPlayed");
    return offsets;
}

int32_t ReadInt(const void* object, int32_t offset) {
    return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(object) + offset);
}

const void* ReadPointer(const void* object, int32_t offset) {
    return *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(object) + offset);
}

// Поиск сущности по индексу — дословно CEntitySystem::GetEntityIdentity из
// entity2/entitysystem.cpp. Своя копия не от хорошей жизни: тот файл в плагин
// не компилируется, на Windows символ не находится, а тянуть ради одной
// функции ещё один исходник SDK со всеми его зависимостями — дороже.
// Здесь только публичные поля заголовка.
CEntityIdentity* FindIdentity(CEntitySystem* entities, int index) {
    if (index < 0 || index >= (MAX_TOTAL_ENTITIES) - 1) return nullptr;

    CEntityIdentity* chunk = entities->m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
    if (chunk == nullptr) return nullptr;

    CEntityIdentity* identity = &chunk[index % MAX_ENTITIES_IN_LIST];
    if (identity->GetEntityIndex() != CEntityIndex(index)) return nullptr;

    return identity;
}

// Состояние, принадлежащее карте
bool g_offsetChecked = false;
bool g_offsetLooksRight = false;
const void* g_gameRules = nullptr;

// Система сущностей — по смещению из gamedata.json.
//
// Это то самое единственное число, зависящее от версии игры. Указатель внутри
// IGameResourceService появляется вместе с картой, поэтому читаем при каждом
// обращении, а не кэшируем при загрузке.
//
// Проверка честная, но не полная: неверное смещение даёт указатель, который
// чаще всего ноль или мусор, и мусор ловится по сущности 0 (worldspawn есть
// на любой карте). Но само первое чтение через мусорный адрес защитить нечем —
// это цена любой gamedata, и у всех нативных плагинов она одна и та же.
CEntitySystem* EntitySystem() {
    if (g_gameResourceService == nullptr) return nullptr;
    if (g_entitySystemOffset <= 0 || g_entitySystemOffset > 4096) return nullptr;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(g_gameResourceService);
    CEntitySystem* entities =
        *reinterpret_cast<CEntitySystem* const*>(base + g_entitySystemOffset);

    if (entities == nullptr || (reinterpret_cast<uintptr_t>(entities) & 7u) != 0) return nullptr;

    if (!g_offsetChecked) {
        g_offsetChecked = true;
        g_offsetLooksRight = FindIdentity(entities, 0) != nullptr;
        if (!g_offsetLooksRight) {
            META_CONPRINTF("[ConnectHistory] [ERROR] [Gamedata] смещение GameEntitySystem (%d) "
                           "не привело к системе сущностей: сущности 0 нет. Итоги матча "
                           "останутся нулями. Обновите gamedata.json и перезапустите сервер\n",
                           g_entitySystemOffset);
        }
    }

    return g_offsetLooksRight ? entities : nullptr;
}

const CEntityInstance* Controller(CEntitySystem* entities, int slot) {
    if (slot < 0) return nullptr;

    // Контроллер игрока — сущность с индексом slot + 1; так его находят
    // и CounterStrikeSharp, и остальные нативные плагины
    CEntityIdentity* identity = FindIdentity(entities, slot + 1);
    return identity != nullptr ? entities->GetEntityInstance(identity) : nullptr;
}

// cs_gamerules — единственная сущность своего класса на карте. Ищем по цепочке
// активных сущностей один раз на карту.
const void* GameRules(CEntitySystem* entities) {
    if (g_gameRules != nullptr) return g_gameRules;

    const Offsets& off = Resolve();
    if (off.gameRules < 0) return nullptr;

    int guard = 0;
    for (CEntityIdentity* identity = entities->m_EntityList.m_pFirstActiveEntity;
         identity != nullptr && guard < MAX_TOTAL_ENTITIES; identity = identity->m_pNext, ++guard) {
        const char* className = identity->GetClassname();
        if (className == nullptr || std::strcmp(className, "cs_gamerules") != 0) continue;
        if (identity->m_pInstance == nullptr) continue;

        g_gameRules = ReadPointer(identity->m_pInstance, off.gameRules);
        break;
    }

    return g_gameRules;
}

}  // namespace

bool ReadController(int slot, MatchStats* out) {
    if (out == nullptr) return false;

    CEntitySystem* entities = EntitySystem();
    if (entities == nullptr) return false;

    const CEntityInstance* controller = Controller(entities, slot);
    if (controller == nullptr) return false;

    const Offsets& off = Resolve();
    if (!off.ControllerComplete()) return false;

    const void* tracking = ReadPointer(controller, off.actionTracking);
    if (tracking == nullptr) return false;

    const uint8_t* match = static_cast<const uint8_t*>(tracking) + off.matchStats;

    out->team = ReadInt(controller, off.teamNum);
    out->score = ReadInt(controller, off.score);
    out->hasScore = true;
    out->mvps = ReadInt(controller, off.mvps);
    out->kills = ReadInt(match, off.kills);
    out->deaths = ReadInt(match, off.deaths);
    out->assists = ReadInt(match, off.assists);
    out->headShots = ReadInt(match, off.headShots);
    out->damage = ReadInt(match, off.damage);
    out->valid = true;
    return true;
}

bool ReadTeam(int slot, int32_t* team) {
    if (team == nullptr) return false;

    CEntitySystem* entities = EntitySystem();
    if (entities == nullptr) return false;

    const CEntityInstance* controller = Controller(entities, slot);
    if (controller == nullptr) return false;

    const Offsets& off = Resolve();
    if (off.teamNum < 0) return false;

    *team = ReadInt(controller, off.teamNum);
    return true;
}

bool ReadRoundsPlayed(int32_t* rounds) {
    if (rounds == nullptr) return false;

    CEntitySystem* entities = EntitySystem();
    if (entities == nullptr) return false;

    const void* rules = GameRules(entities);
    if (rules == nullptr) return false;

    const Offsets& off = Resolve();
    if (off.roundsPlayed < 0) return false;

    *rounds = ReadInt(rules, off.roundsPlayed);
    return true;
}

void ForgetMap() {
    g_gameRules = nullptr;
    g_offsetChecked = false;
    g_offsetLooksRight = false;
}

}  // namespace stats
}  // namespace ch
