#include "mm/stats.h"

#include "core/jobs.h"
#include "mm/globals.h"
#include "mm/schema.h"
#include "mm/utils_api.h"

#include <entity2/entityidentity.h>
#include <entity2/entitysystem.h>

#include <cstdint>

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

    bool Complete() const {
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
    return offsets;
}

// Поиск сущности по индексу — дословно CEntitySystem::GetEntityIdentity из
// entity2/entitysystem.cpp. Своя копия не от хорошей жизни: тот файл в плагин
// не компилируется, на Windows символ не находится, а тянуть ради одной
// функции ещё один исходник SDK со всеми его зависимостями — дороже.
// Здесь только публичные поля заголовка, никакой gamedata.
CEntityIdentity* FindIdentity(CEntitySystem* entities, int index) {
    if (index < 0 || index >= (MAX_TOTAL_ENTITIES) - 1) return nullptr;

    CEntityIdentity* chunk = entities->m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
    if (chunk == nullptr) return nullptr;

    CEntityIdentity* identity = &chunk[index % MAX_ENTITIES_IN_LIST];
    if (identity->GetEntityIndex() != CEntityIndex(index)) return nullptr;

    return identity;
}

int32_t ReadInt(const void* object, int32_t offset) {
    return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(object) + offset);
}

const void* ReadPointer(const void* object, int32_t offset) {
    return *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(object) + offset);
}

}  // namespace

bool ReadController(int slot, MatchStats* out) {
    if (out == nullptr || slot < 0 || g_utils == nullptr) return false;

    // Систему сущностей спрашиваем каждый раз, а не кэшируем: она живёт
    // столько же, сколько загруженная карта.
    CEntitySystem* entities = g_utils->GetCEntitySystem();
    if (entities == nullptr) return false;

    // Контроллер игрока — сущность с индексом slot + 1; так его находят
    // и CounterStrikeSharp, и плагины Pisex
    CEntityIdentity* identity = FindIdentity(entities, slot + 1);
    const CEntityInstance* controller =
        identity != nullptr ? entities->GetEntityInstance(identity) : nullptr;
    if (controller == nullptr) return false;

    const Offsets& off = Resolve();
    if (!off.Complete()) return false;

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

}  // namespace stats
}  // namespace ch
