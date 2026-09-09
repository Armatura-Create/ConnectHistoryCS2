// Указатели на интерфейсы движка, полученные в Load().
//
// Всё, что здесь лежит, доступно ТОЛЬКО из главного потока. Обращение к любому
// из этих указателей из фонового потока — чтение чужой памяти и смерть процесса
// без стека в логе.
#pragma once

class IVEngineServer2;
class ISource2Server;
class IGameEventSystem;
class INetworkMessages;
class ISchemaSystem;
// Именно ISource2GameClients: IServerGameClients в eiface.h — typedef, и
// форвард-объявление его классом ломает сборку раньше первой полезной ошибки.
class ISource2GameClients;

namespace ch {

extern IVEngineServer2* g_engine;
extern ISource2Server* g_server;

// Доставка UserMessage в чат (см. mm/chat.cpp). Оба — фабричные интерфейсы,
// сигнатур и смещений для чата не требуется.
extern IGameEventSystem* g_gameEventSystem;
extern INetworkMessages* g_networkMessages;

// Схема игры: смещения полей контроллера по именам (см. mm/schema.cpp).
// Фабричный интерфейс, как и остальные.
extern ISchemaSystem* g_schemaSystem;

namespace pisex {
class IUtilsApi;
}

// Плагин Utils от Pisex, если стоит на сервере. nullptr — нет, и это штатно:
// без него плагин работает, но итоги матча остаются нулями (см. mm/utils_api.h).
extern pisex::IUtilsApi* g_utils;

// ICvar своего указателя не имеет специально: интерфейс забирается в g_pCVar
// из tier1, потому что ConVar_Register (а значит и META_CONVAR_REGISTER)
// смотрит именно туда. Свой второй указатель молча оставил бы регистрацию
// команд работать по нулю.
extern ISource2GameClients* g_gameClients;

}  // namespace ch
