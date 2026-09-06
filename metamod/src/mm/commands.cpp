// Команды плагина.
//
// Админские существуют, чтобы петля «что-то не пишется → в чём причина» была
// секундой, а не чтением логов: ch_status показывает связь с базой, очередь
// и последнюю ошибку.
//
// Права проверяются просто: все команды доступны только с консоли сервера.
// Своей системы прав у Metamod нет, а изобретать её ради четырёх команд значило бы
// завести ещё один файл с правами, который разъедется с настоящим.
//
// ОГРАНИЧЕНИЕ ЭТОЙ ЦЕЛИ. В C#-версиях ch_playtime и ch_lastseen — команды ИГРОКА,
// и ответ уходит ему в чат. Здесь они консольные и принимают SteamID аргументом:
// отправка сообщения конкретному игроку в CS2 идёт через UserMessage с протобуфами
// SDK, и проверить такую сборку без живого сервера невозможно. Отдавать вместо
// этого «почти работающую» команду, печатающую ответ не туда, — хуже, чем честно
// иметь консольную. Тексты, подстановки и Messages.json при этом уже общие
// с остальными целями.
#include "core/config.h"
#include "core/logger.h"
#include "core/query_service.h"
#include "core/util/chat_format.h"
#include "core/util/timeutil.h"
#include "mm/globals.h"
#include "mm/plugin.h"

#include <convar.h>
#include <eiface.h>

#include <algorithm>
#include <cstdlib>
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

CON_COMMAND_F(ch_playtime, "ch_playtime <steamid64> — наигранное время игрока",
              FCVAR_GAMEDLL | FCVAR_RELEASE) {
    if (context.GetPlayerSlot().Get() != -1) return;
    if (args.ArgC() < 2) {
        META_CONPRINTF("[ConnectHistory] Использование: ch_playtime <steamid64>\n");
        return;
    }
    g_plugin.CommandPlaytime(std::strtoull(args.Arg(1), nullptr, 10));
}

CON_COMMAND_F(ch_lastseen, "ch_lastseen <steamid64> — последние заходы игрока",
              FCVAR_GAMEDLL | FCVAR_RELEASE) {
    if (context.GetPlayerSlot().Get() != -1) return;
    if (args.ArgC() < 2) {
        META_CONPRINTF("[ConnectHistory] Использование: ch_lastseen <steamid64>\n");
        return;
    }
    g_plugin.CommandLastSeen(std::strtoull(args.Arg(1), nullptr, 10));
}

}  // namespace

void ConnectHistoryPlugin::RegisterPluginCommands() {
    // CON_COMMAND_F регистрирует команды сам при загрузке модуля; отдельной
    // регистрации не требуется. Метод оставлен точкой расширения и симметрии
    // с Unload.
}

void ConnectHistoryPlugin::UnregisterPluginCommands() {}

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

// Ответ команды. Цветовые коды из шаблона в консоли не нужны — снимаем их,
// иначе в лог сервера уедут управляющие байты.
void ConnectHistoryPlugin::SendChat(uint64_t steamId, const std::string& message) {
    (void)steamId;

    std::string plain;
    plain.reserve(message.size());
    for (const char c : message) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 0x01 && byte <= 0x10) continue;
        plain.push_back(c);
    }

    META_CONPRINTF("[ConnectHistory] %s\n", plain.c_str());
}

void ConnectHistoryPlugin::CommandPlaytime(uint64_t steamId) {
    if (!_config.commands.playerCommandsEnabled || !_query) return;

    const int64_t now = UtcNowSeconds();
    const int cooldown = std::max(0, _config.commands.cooldownSeconds);
    if (cooldown > 0) {
        const auto next = _commandCooldown.find(steamId);
        if (next != _commandCooldown.end() && next->second > now) return;
        _commandCooldown[steamId] = now + cooldown;
    }

    const PlayerTotals totals = _query->GetTotals(steamId);
    const std::string lang = _config.defaultLang;

    std::map<std::string, std::string> values;
    values["{prefix}"] = Localize("prefix", lang);

    if (!totals.found) {
        SendChat(steamId, chat::Render(Localize("no_data", lang), values));
        return;
    }

    values["{TOTAL}"] = FormatDuration(totals.totalSeconds);
    values["{SESSIONS}"] = std::to_string(totals.sessions);
    values["{FIRST}"] = FormatDisplayDateTime(totals.firstSeen, _displayOffsetSeconds);

    SendChat(steamId, chat::Render(Localize("playtime", lang), values));
}

void ConnectHistoryPlugin::CommandLastSeen(uint64_t steamId) {
    if (!_config.commands.playerCommandsEnabled || !_query) return;

    const std::vector<RecentSession> sessions =
        _query->GetRecent(steamId, _config.commands.lastSeenLimit);
    const std::string lang = _config.defaultLang;

    std::map<std::string, std::string> values;
    values["{prefix}"] = Localize("prefix", lang);

    if (sessions.empty()) {
        SendChat(steamId, chat::Render(Localize("no_data", lang), values));
        return;
    }

    SendChat(steamId, chat::Render(Localize("lastseen_header", lang), values));

    for (const RecentSession& session : sessions) {
        std::map<std::string, std::string> row = values;
        row["{DATE}"] = FormatDisplayDateTime(session.startedAt, _displayOffsetSeconds);
        row["{DURATION}"] = FormatDuration(session.durationSeconds);
        row["{MAP}"] = session.map;

        SendChat(steamId, chat::Render(Localize("lastseen_row", lang), row));
    }
}

}  // namespace ch
