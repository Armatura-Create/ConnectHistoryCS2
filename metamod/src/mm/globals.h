// Указатели на интерфейсы движка, полученные в Load().
//
// Всё, что здесь лежит, доступно ТОЛЬКО из главного потока. Обращение к любому
// из этих указателей из фонового потока — чтение чужой памяти и смерть процесса
// без стека в логе.
#pragma once

class IVEngineServer2;
class ICvar;
class ISource2Server;
// Именно ISource2GameClients: IServerGameClients в eiface.h — typedef, и
// форвард-объявление его классом ломает сборку раньше первой полезной ошибки.
class ISource2GameClients;

namespace ch {

extern IVEngineServer2* g_engine;
extern ICvar* g_cvar;
extern ISource2Server* g_server;
extern ISource2GameClients* g_gameClients;

}  // namespace ch
