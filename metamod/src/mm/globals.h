// Указатели на интерфейсы движка, полученные в Load().
//
// Всё, что здесь лежит, доступно ТОЛЬКО из главного потока. Обращение к любому
// из этих указателей из фонового потока — чтение чужой памяти и смерть процесса
// без стека в логе.
#pragma once

class IVEngineServer2;
class ICvar;
class ISource2Server;
class IServerGameClients;
class IGameEventManager2;

namespace ch {

extern IVEngineServer2* g_engine;
extern ICvar* g_cvar;
extern ISource2Server* g_server;
extern IServerGameClients* g_gameClients;

// Забирается хуком IGameEventManager2::LoadEventsFromFile: прямого способа
// получить менеджер событий в CS2 нет, а сигнатуры и смещения мы принципиально
// не используем.
extern IGameEventManager2* g_gameEventManager;

}  // namespace ch
