// Игровые события. Единственная точка регистрации слушателей.
//
// Это и есть источник итогов матча: убийства, смерти, помощь, урон, MVP и раунды
// приходят событиями, а не читаются из полей контроллера. Чтение контроллера
// потребовало бы указателя на CGameEntitySystem, добываемого захардкоженным
// смещением, — то есть падения сервера в очередное обновление игры.
#include "core/session.h"
#include "mm/globals.h"
#include "mm/plugin.h"

#include "core/util/timeutil.h"

#include <igameevents.h>

#include <vector>

namespace ch {
namespace {

using EventCallback = void (*)(IGameEvent* event);

// Слушатель одного события. Тот же приём, что в CS2Fixes: свой объект на событие,
// имя строкой, никаких сигнатур.
class GameEventListener final : public IGameEventListener2 {
public:
    GameEventListener(EventCallback callback, const char* name)
        : _callback(callback), _name(name) {}

    void FireGameEvent(IGameEvent* event) override { _callback(event); }

    const char* Name() const { return _name; }

private:
    EventCallback _callback;
    const char* _name;
};

// SteamID участника события. 0 означает «не игрок или уже отключился» —
// такие события просто пропускаются.
uint64_t SteamIdOf(IGameEvent* event, const char* key) {
    if (event == nullptr) return 0;
    return event->GetUint64(key, 0);
}

void OnRoundEnd(IGameEvent*) { g_plugin.OnRoundEnd(); }

void OnPlayerTeam(IGameEvent* event) {
    // Ботов пропускаем: у них нет SteamID, и в историю они не попадают
    if (event->GetBool("isbot", false)) return;
    g_plugin.OnPlayerTeam(SteamIdOf(event, "userid_steamid"), event->GetInt("team", 0));
}

void OnPlayerDeath(IGameEvent* event) {
    g_plugin.OnPlayerDeath(SteamIdOf(event, "attacker_steamid"),
                           SteamIdOf(event, "userid_steamid"),
                           SteamIdOf(event, "assister_steamid"),
                           event->GetBool("headshot", false));
}

void OnPlayerHurt(IGameEvent* event) {
    g_plugin.OnPlayerHurt(SteamIdOf(event, "attacker_steamid"),
                          event->GetInt("dmg_health", 0));
}

void OnRoundMvp(IGameEvent* event) {
    g_plugin.OnRoundMvp(SteamIdOf(event, "userid_steamid"));
}

std::vector<GameEventListener*>& Listeners() {
    static std::vector<GameEventListener*> listeners;
    return listeners;
}

}  // namespace

void ConnectHistoryPlugin::RegisterEventListeners() {
    if (g_gameEventManager == nullptr || !Listeners().empty()) return;

    Listeners().push_back(new GameEventListener(OnRoundEnd, "round_end"));
    Listeners().push_back(new GameEventListener(OnPlayerTeam, "player_team"));
    Listeners().push_back(new GameEventListener(OnPlayerDeath, "player_death"));
    Listeners().push_back(new GameEventListener(OnPlayerHurt, "player_hurt"));
    Listeners().push_back(new GameEventListener(OnRoundMvp, "round_mvp"));

    for (GameEventListener* listener : Listeners()) {
        g_gameEventManager->AddListener(listener, listener->Name(), true);
    }
}

void ConnectHistoryPlugin::UnregisterEventListeners() {
    if (g_gameEventManager != nullptr) {
        for (GameEventListener* listener : Listeners()) {
            g_gameEventManager->RemoveListener(listener);
        }
    }

    for (GameEventListener* listener : Listeners()) delete listener;
    Listeners().clear();
}

// Раунды считаем событием, а не чтением схемы движка: нам нужно то, что игрок
// застал в ЭТОЙ сессии, а поля контроллера обнуляются сменой карты.
void ConnectHistoryPlugin::OnRoundEnd() {
    _sessions.ForEach([](OpenSession& session) { session.NoteRoundEnd(); });
}

void ConnectHistoryPlugin::OnPlayerTeam(uint64_t steamId, int team) {
    if (steamId == 0) return;
    const int64_t now = UtcNowSeconds();
    _sessions.Update(steamId, [team, now](OpenSession& session) {
        session.NoteTeam(team, now);
    });
}

void ConnectHistoryPlugin::OnPlayerDeath(uint64_t attacker, uint64_t victim,
                                         uint64_t assister, bool headshot) {
    // Самоубийство и урон от мира не считаем убийством: attacker равен жертве
    // или отсутствует
    if (attacker != 0 && attacker != victim) {
        _sessions.Update(attacker,
                         [headshot](OpenSession& session) { session.NoteKill(headshot); });
    }
    if (victim != 0) {
        _sessions.Update(victim, [](OpenSession& session) { session.NoteDeath(); });
    }
    if (assister != 0 && assister != victim) {
        _sessions.Update(assister, [](OpenSession& session) { session.NoteAssist(); });
    }
}

void ConnectHistoryPlugin::OnPlayerHurt(uint64_t attacker, int damageHealth) {
    if (attacker == 0) return;
    _sessions.Update(attacker, [damageHealth](OpenSession& session) {
        session.NoteDamage(damageHealth);
    });
}

void ConnectHistoryPlugin::OnRoundMvp(uint64_t steamId) {
    if (steamId == 0) return;
    _sessions.Update(steamId, [](OpenSession& session) { session.NoteMvp(); });
}

}  // namespace ch
