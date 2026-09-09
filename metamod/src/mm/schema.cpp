#include "mm/schema.h"

#include "mm/globals.h"
// plugin.h ради PLUGIN_GLOBALVARS: META_CONPRINTF — макрос поверх g_SMAPI
#include "mm/plugin.h"

#include <schemasystem/schemasystem.h>

#include <cstring>
#include <map>
#include <string>

namespace ch {
namespace schema {
namespace {

#ifdef _WIN32
constexpr const char* kServerModule = "server.dll";
#else
constexpr const char* kServerModule = "libserver.so";
#endif

// Поле ищется в классе и вверх по цепочке одиночного наследования: m_iTeamNum
// объявлен в CBaseEntity, а спрашивают его у CCSPlayerController; m_iKills —
// в CSPerRoundStats_t, а спрашивают у CSMatchStats_t. Смещение базового
// класса (m_nOffset) прибавляется — при одиночном наследовании это ноль,
// но полагаться на это молча незачем.
int32_t FindInClass(const SchemaClassInfoData_t* info, const char* fieldName, int32_t base) {
    while (info != nullptr) {
        for (uint16_t i = 0; i < info->m_nFieldCount; ++i) {
            const SchemaClassFieldData_t& field = info->m_pFields[i];
            if (field.m_pszName != nullptr && std::strcmp(field.m_pszName, fieldName) == 0) {
                return base + field.m_nSingleInheritanceOffset;
            }
        }

        if (info->m_nBaseClassCount == 0 || info->m_pBaseClasses == nullptr) return -1;

        base += static_cast<int32_t>(info->m_pBaseClasses[0].m_nOffset);
        info = info->m_pBaseClasses[0].m_pClass;
    }
    return -1;
}

}  // namespace

int32_t FieldOffset(const char* className, const char* fieldName) {
    static std::map<std::string, int32_t> cache;

    const std::string key = std::string(className) + "::" + fieldName;
    const auto known = cache.find(key);
    if (known != cache.end()) return known->second;

    int32_t offset = -1;

    if (g_schemaSystem != nullptr) {
        CSchemaSystemTypeScope* scope = g_schemaSystem->FindTypeScopeForModule(kServerModule);
        if (scope != nullptr) {
            const SchemaClassInfoData_t* info = scope->FindDeclaredClass(className).Get();
            if (info != nullptr) offset = FindInClass(info, fieldName, 0);
        }
    }

    // Один раз: поле спрашивают при каждом выходе игрока, и лог не должен
    // превращаться в одну и ту же строку на каждого
    if (offset < 0) {
        META_CONPRINTF("[ConnectHistory] [WARN] [Schema] %s::%s не найдено — итоги матча "
                       "по этому полю останутся нулём\n",
                       className, fieldName);
    }

    cache[key] = offset;
    return offset;
}

}  // namespace schema
}  // namespace ch
