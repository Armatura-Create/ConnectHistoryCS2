// Команды плагина.
//
// Админские существуют, чтобы петля «что-то не пишется → в чём причина» была
// секундой, а не чтением логов: ch_status показывает связь с базой, очередь
// и последнюю ошибку. Они доступны только с консоли сервера: своей системы прав
// у Metamod нет, а изобретать её ради двух команд значило бы завести ещё один
// файл с правами, который разъедется с настоящим.
//
// Игроцкие — ch_playtime и ch_lastseen — работают и из чата (!playtime,
// /playtime), и из консоли клиента, и отвечают В ЧАТ, как в целях на C#.
// Раньше здесь стояло, что так нельзя: «отправка сообщения конкретному игроку
// идёт через UserMessage с протобуфами SDK, и проверить такую сборку без живого
// сервера невозможно». Первая половина верна, вывод — нет: тип сообщения отдаёт
// INetworkMessages, доставку делает IGameEventSystem, чат слышен через хук
// ICvar::DispatchConCommand. Всё это фабричные интерфейсы, ни одного смещения.
//
// С консоли сервера те же команды принимают SteamID аргументом и отвечают
// в консоль — так админ спрашивает про игрока, которого сейчас нет на сервере.
#include "core/config.h"
#include "core/logger.h"
#include "core/query_service.h"
#include "core/util/chat_format.h"
#include "core/util/timeutil.h"
#include "mm/chat.h"
#include "mm/globals.h"
#include "mm/plugin.h"

#include <convar.h>
#include <eiface.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace ch {
namespace {

CON_COMMAND_F(ch_status, "Состояние ConnectHistory: база, очередь, последняя ошибка",
              FCVAR_GAMEDLL | FCVAR_RELEASE) {
    // Только с консоли сервера: у команды нет проверки прав, а показывает она
    // адрес базы и текст последней ошибки
    if (context.GetPlayerSlot().Get() != -1) return;
    g_plugin.CommandStatus();
}

CON_COMMAND_F(ch_reload, "Перечитать конфигурацию ConnectHistory",
              FCVAR_GAMEDLL | FCVAR_RELEASE) {
    if (context.GetPlayerSlot().Get() != -1) return;
    g_plugin.CommandReload();
}

// Обе команды работают с двух сторон: игрок спрашивает про себя и получает
// ответ в чат, консоль сервера спрашивает про кого угодно по SteamID.
uint64_t TargetSteamId(const CCommandContext& context, const CCommand& args,
                       const char* usage) {
    const int slot = context.GetPlayerSlot().Get();
    if (slot >= 0) return g_plugin.SteamIdForSlot(slot);

    if (args.ArgC() < 2) {
        META_CONPRINTF("[ConnectHistory] Использование: %s <steamid64>\n", usage);
        return 0;
    }
    return std::strtoull(args.Arg(1), nullptr, 10);
}

CON_COMMAND_F(ch_playtime, "ch_playtime [steamid64] — наигранное время игрока",
              FCVAR_GAMEDLL | FCVAR_RELEASE) {
    const uint64_t steamId = TargetSteamId(context, args, "ch_playtime");
    if (steamId != 0) g_plugin.CommandPlaytime(steamId, context.GetPlayerSlot().Get());
}

CON_COMMAND_F(ch_lastseen, "ch_lastseen [steamid64] — последние заходы игрока",
              FCVAR_GAMEDLL | FCVAR_RELEASE) {
    const uint64_t steamId = TargetSteamId(context, args, "ch_lastseen");
    if (steamId != 0) g_plugin.CommandLastSeen(steamId, context.GetPlayerSlot().Get());
}

}  // namespace

void ConnectHistoryPlugin::RegisterPluginCommands() {
    // На Source 2 объект ConCommand, созданный CON_COMMAND_F при загрузке
    // модуля, движку ещё не известен: его отдаёт ConVar_Register. Плагин
    // Metamod обязан вызвать её через META_CONVAR_REGISTER, чтобы MM:S знал,
    // чьи это команды и что снимать при выгрузке. Без этого ch_status просто
    // не существует в консоли.
    META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);
}

void ConnectHistoryPlugin::UnregisterPluginCommands() {
    // Симметрия обязательна: команды живут в выгружаемой библиотеке, и
    // оставленный движку указатель на неё — падение при следующем вызове.
    ConVar_Unregister();
}

void ConnectHistoryPlugin::CommandStatus() {
    META_CONPRINTF("[ConnectHistory] %s\n", _version.c_str());
    META_CONPRINTF("  Сервер #%d, открытых сессий: %zu\n", _config.serverId,
                   _sessions.Count());
    META_CONPRINTF("  База: %s\n",
                   _database ? _database->Target().c_str() : "не настроена");
    META_CONPRINTF("  Конфиг: %s\n", _configDirectory.c_str());

    if (!_writer) {
        META_CONPRINTF("  Писатель не запущен\n");
        return;
    }

    META_CONPRINTF("  Очередь: %zu, записано: %lld, в спуле: %lld%s\n", _writer->Pending(),
                   static_cast<long long>(_writer->Written()),
                   static_cast<long long>(_writer->Spooled()),
                   _writer->SpoolExists() ? " (файл спула есть)" : "");

    const std::string lastError = _writer->LastError();
    META_CONPRINTF("  Последняя ошибка: %s\n",
                   lastError.empty() ? "нет" : lastError.c_str());

    // Проверка связи ходит по сети. Здесь это допустимо: команда выполняется
    // с консоли сервера, а не в игровом кадре под нагрузкой.
    if (_database) {
        std::string message;
        const bool ok = _database->Ping(&message);
        META_CONPRINTF("  Связь с базой: %s (%s)\n", ok ? "OK" : "ОШИБКА",
                       message.c_str());
    }
}

void ConnectHistoryPlugin::CommandReload() {
    // Открытые сессии закрываем ДО пересоздания писателя: иначе их закрытие
    // уедет в очередь, которую мы тут же выбросим.
    CloseAllSessions(SessionEndKind::PluginUnload);

    _writer.reset();
    _query.reset();
    _database.reset();

    ReloadConfig();
    BuildDatabaseStack();
    RegisterServer();

    META_CONPRINTF("[ConnectHistory] Конфигурация перезагружена\n");
}

std::string ConnectHistoryPlugin::Localize(const std::string& key,
                                           const std::string& lang) const {
    const auto entry = _config.messages.find(key);
    if (entry == _config.messages.end() || entry->second.empty()) return std::string();

    const auto exact = entry->second.find(lang);
    if (exact != entry->second.end()) return exact->second;

    const auto fallback = entry->second.find(_config.defaultLang);
    if (fallback != entry->second.end()) return fallback->second;

    return entry->second.begin()->second;
}

// Ответ команды.
//
// Игроку — в чат, через EnsureChatColorPrefix: движок CS2 съедает цветовой код,
// стоящий в самом начале строки, и без этой обёртки "{GREEN}[История] ..."
// вышло бы белым. Инвариант из CLAUDE.md — в чат только через неё.
//
// Консоли (и игроку, до которого не достучались) — тот же текст без цветов:
// управляющие байты в логе сервера читать невозможно.
void ConnectHistoryPlugin::SendChat(int slot, const std::string& message) {
    if (slot >= 0 && chatmsg::SendToSlot(slot, chat::EnsureChatColorPrefix(message))) {
        return;
    }

    std::string plain;
    plain.reserve(message.size());
    for (const char c : message) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 0x01 && byte <= 0x10) continue;
        plain.push_back(c);
    }

    META_CONPRINTF("[ConnectHistory] %s\n", plain.c_str());
}

// Разбор "!playtime" / "/lastseen" из чата.
//
// true означает «команда наша» — вызывающий не пускает такое сообщение в общий
// чат. Всё остальное уходит нетронутым: плагин истории не имеет права глотать
// чужие сообщения.
bool ConnectHistoryPlugin::HandleChatCommand(int slot, const char* text) {
    if (text == nullptr) return false;
    if (*text != '!' && *text != '/') return false;

    ++text;

    // Берём первое слово: аргументов у этих команд нет, но человек может
    // дописать что угодно после пробела
    std::string name;
    while (*text != '\0' && *text != ' ') {
        const char c = *text++;
        name.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }

    const uint64_t steamId = SteamIdForSlot(slot);
    if (steamId == 0) return false;

    if (name == "playtime") {
        CommandPlaytime(steamId, slot);
        return true;
    }
    if (name == "lastseen") {
        CommandLastSeen(steamId, slot);
        return true;
    }
    return false;
}

void ConnectHistoryPlugin::CommandPlaytime(uint64_t steamId, int slot) {
    if (!_config.commands.playerCommandsEnabled || !_query) return;

    const int64_t now = UtcNowSeconds();
    const int cooldown = std::max(0, _config.commands.cooldownSeconds);
    if (cooldown > 0) {
        const auto next = _commandCooldown.find(steamId);
        if (next != _commandCooldown.end() && next->second > now) return;
        _commandCooldown[steamId] = now + cooldown;
    }

    const PlayerTotals totals = _query->GetTotals(steamId);
    const std::string lang = ClientLanguage(slot);

    std::map<std::string, std::string> values;
    values["{prefix}"] = Localize("prefix", lang);

    if (!totals.found) {
        SendChat(slot, chat::Render(Localize("no_data", lang), values));
        return;
    }

    values["{TOTAL}"] = FormatDuration(totals.totalSeconds);
    values["{SESSIONS}"] = std::to_string(totals.sessions);
    values["{FIRST}"] = FormatDisplayDateTime(totals.firstSeen, _displayOffsetSeconds);

    SendChat(slot, chat::Render(Localize("playtime", lang), values));
}

void ConnectHistoryPlugin::CommandLastSeen(uint64_t steamId, int slot) {
    if (!_config.commands.playerCommandsEnabled || !_query) return;

    const std::vector<RecentSession> sessions =
        _query->GetRecent(steamId, _config.commands.lastSeenLimit);
    const std::string lang = ClientLanguage(slot);

    std::map<std::string, std::string> values;
    values["{prefix}"] = Localize("prefix", lang);

    if (sessions.empty()) {
        SendChat(slot, chat::Render(Localize("no_data", lang), values));
        return;
    }

    SendChat(slot, chat::Render(Localize("lastseen_header", lang), values));

    for (const RecentSession& session : sessions) {
        std::map<std::string, std::string> row = values;
        row["{DATE}"] = FormatDisplayDateTime(session.startedAt, _displayOffsetSeconds);
        row["{DURATION}"] = FormatDuration(session.durationSeconds);
        row["{MAP}"] = session.map;

        SendChat(slot, chat::Render(Localize("lastseen_row", lang), row));
    }
}

}  // namespace ch
