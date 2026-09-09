// Интерфейс плагина Utils (Pisex, https://github.com/Pisex/cs2-menus).
//
// Зачем он нам. Итоги матча живут в полях контроллера игрока, а до контроллера
// нужен указатель на CEntitySystem, который в CS2 не отдаёт ни одна фабрика —
// его добывают по gamedata. Этот плагин обещает не иметь ни одного смещения,
// и держит обещание так: смещение живёт в Utils, который на серверах с
// плагинами Pisex (ranks, vip, admin) и так стоит. Utils же отдаёт игровые
// события — IGameEventManager2 нам тоже недоступен без сигнатур.
//
// Зависимость НЕОБЯЗАТЕЛЬНАЯ: без Utils плагин работает как раньше, только
// итоги матча остаются нулями, а score — NULL.
//
// ЭТО ЗЕРКАЛО ЧУЖОЙ VTABLE. Порядок и сигнатуры методов повторяют include/menus.h
// из cs2-menus один в один до ClearAllHooks включительно; хвост интерфейса
// нам не нужен, и его отсутствие безопасно — лишние виртуальные методы в конце
// настоящего объекта ничему не мешают. Виртуального деструктора у оригинала
// нет, и здесь его быть не должно: он сдвинул бы всю таблицу на одну позицию.
#pragma once

#include <ISmmPlugin.h>

#include <functional>
#include <string>
#include <vector>

class CBaseEntity;
class CCSGameRules;
class CEntitySystem;
class CGameEntitySystem;
class CGlobalVars;
class IGameEvent;
class IGameEventManager2;

namespace ch {
namespace pisex {

constexpr const char* kUtilsInterface = "IUtilsApi";

using CommandCallback = std::function<bool(int slot, const char* content)>;
using CommandCallbackPre = std::function<bool(int slot, const char* content, bool team)>;
using CommandCallbackPost =
    std::function<bool(int slot, const char* content, bool mute, bool team)>;
using EventCallback =
    std::function<void(const char* name, IGameEvent* event, bool dontBroadcast)>;
using StartupCallback = std::function<void()>;

class IUtilsApi {
public:
    virtual void PrintToChat(int slot, const char* msg, ...) = 0;
    virtual void PrintToChatAll(const char* msg, ...) = 0;
    virtual void NextFrame(std::function<void()> fn) = 0;
    virtual CCSGameRules* GetCCSGameRules() = 0;
    virtual CGameEntitySystem* GetCGameEntitySystem() = 0;
    virtual CEntitySystem* GetCEntitySystem() = 0;
    virtual CGlobalVars* GetCGlobalVars() = 0;
    virtual IGameEventManager2* GetGameEventManager() = 0;

    virtual const char* GetLanguage() = 0;

    virtual void StartupServer(PluginId id, StartupCallback fn) = 0;
    virtual void OnGetGameRules(PluginId id, StartupCallback fn) = 0;

    virtual void RegCommand(PluginId id, const std::vector<std::string>& console,
                            const std::vector<std::string>& chat,
                            const CommandCallback& callback) = 0;
    virtual void AddChatListenerPre(PluginId id, CommandCallbackPre callback) = 0;
    virtual void AddChatListenerPost(PluginId id, CommandCallbackPost callback) = 0;
    virtual void HookEvent(PluginId id, const char* name, EventCallback callback) = 0;

    virtual void SetStateChanged(CBaseEntity* entity, const char* className,
                                 const char* fieldName, int extraOffset = 0) = 0;

    // Снимает всё, что мы навесили, — обязательно при выгрузке: колбэки
    // указывают внутрь нашей библиотеки.
    virtual void ClearAllHooks(PluginId id) = 0;
};

}  // namespace pisex
}  // namespace ch
