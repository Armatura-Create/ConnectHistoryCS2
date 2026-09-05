using System.IO;
using System.Text;

namespace ConnectHistory;

/// JSON Schema для конфигов.
///
/// Схема — основной способ объяснить конфиг: редактор с её поддержкой подсказывает поля
/// и подсвечивает опечатки, и она не устаревает молча, потому что перезаписывается
/// при каждой загрузке. Правишь модель конфига — правь схему рядом.
public sealed partial class ConfigService
{
    private static void WriteSchemas(string directory)
    {
        File.WriteAllText(Path.Combine(directory, "Settings.schema.json"), SettingsSchema, Encoding.UTF8);
        File.WriteAllText(Path.Combine(directory, "Messages.schema.json"), MessagesSchema, Encoding.UTF8);
    }

    private const string SettingsSchema = """
        {
          "$schema": "https://json-schema.org/draft-07/schema#",
          "title": "ConnectHistory Settings",
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "$schema": { "type": "string" },
            "Debug": {
              "type": "boolean",
              "description": "Подробный лог. Пишет SteamID, ники и IP игроков — по умолчанию выключен."
            },
            "ServerId": {
              "type": "integer",
              "minimum": 1,
              "description": "Номер сервера в таблице ch_servers. У разных серверов обязан отличаться."
            },
            "DefaultLang": {
              "type": "string",
              "description": "Язык сообщений команд по умолчанию (ключ в Messages.json)."
            },
            "DisplayTimeZone": {
              "type": "string",
              "description": "Часовой пояс для времени, которое видят игроки: IANA-идентификатор (Europe/Moscow), \"UTC\" или \"Local\". В базу время всегда пишется в UTC.",
              "examples": ["UTC", "Local", "Europe/Moscow", "Europe/Kyiv", "Asia/Almaty"]
            },
            "Server": {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "PublicAddress": {
                  "type": "string",
                  "description": "Публичный адрес сервера: \"ip:port\" или \"host:port\". Пусто — определять автоматически (ConVar ip часто отдаёт 0.0.0.0, такое значение отбраковывается).",
                  "examples": ["", "203.0.113.10:27015", "cs2.example.com:27015"]
                }
              }
            },
            "Database": {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "Host": { "type": "string" },
                "Port": { "type": "integer", "minimum": 1, "maximum": 65535 },
                "Database": { "type": "string" },
                "User": { "type": "string" },
                "Password": { "type": "string", "description": "Файл держится с правами 600." },
                "SslMode": {
                  "type": "string",
                  "enum": ["None", "Preferred", "Required", "VerifyCA", "VerifyFull"],
                  "description": "Required и выше для базы вне localhost: иначе трафик открытым текстом."
                },
                "TablePrefix": {
                  "type": "string",
                  "pattern": "^[A-Za-z0-9_]{0,16}$",
                  "description": "Префикс имён таблиц. Только латиница, цифры и '_'."
                },
                "MaxPoolSize": { "type": "integer", "minimum": 1, "maximum": 64 },
                "ConnectionTimeoutSeconds": { "type": "integer", "minimum": 1 },
                "CommandTimeoutSeconds": { "type": "integer", "minimum": 1 }
              }
            },
            "Collect": {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "GeoIp": { "type": "boolean", "description": "Страна и город по GeoLite2." },
                "PlayerIp": { "type": "boolean", "description": "Полный IP игрока. Персональные данные." },
                "IpHash": { "type": "boolean", "description": "HMAC от IP вместо адреса." },
                "IpHashSalt": { "type": "string", "description": "Секрет для IpHash. Пустой = хеширование выключено." },
                "MatchStats": { "type": "boolean" },
                "Ping": { "type": "boolean" },
                "CountSpectatorTime": {
                  "type": "boolean",
                  "description": "Считать ли в наигранное время (ch_players.total_seconds) время в наблюдателях и без команды. false — вычитать. На ch_sessions.duration_seconds не влияет: там всегда время подключения."
                },
                "NicknameHistory": { "type": "boolean" },
                "PlayerAggregates": { "type": "boolean" },
                "OnlineSnapshots": { "type": "boolean" },
                "OnlineSnapshotIntervalSeconds": { "type": "integer", "minimum": 30 },
                "PingSampleIntervalSeconds": { "type": "integer", "minimum": 5 }
              }
            },
            "Storage": {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "SpoolEnabled": { "type": "boolean", "description": "Копить записи на диске, пока база недоступна." },
                "SpoolMaxEntries": { "type": "integer", "minimum": 100 },
                "RetryAttempts": { "type": "integer", "minimum": 1, "maximum": 10 },
                "RetryDelaySeconds": { "type": "integer", "minimum": 1 }
              }
            },
            "Commands": {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "PlayerCommandsEnabled": { "type": "boolean" },
                "CooldownSeconds": { "type": "integer", "minimum": 0 },
                "LastSeenLimit": { "type": "integer", "minimum": 1, "maximum": 20 }
              }
            }
          }
        }
        """;

    private const string MessagesSchema = """
        {
          "$schema": "https://json-schema.org/draft-07/schema#",
          "title": "ConnectHistory Messages",
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "$schema": { "type": "string" },
            "Messages": {
              "type": "object",
              "description": "Ключ сообщения -> язык -> текст. Поддерживаются теги цвета {GREEN}, {RED}, {GOLD}, {LIGHTBLUE}, {GREY}, {DEFAULT} и подстановка {prefix}.",
              "additionalProperties": {
                "type": "object",
                "additionalProperties": { "type": "string" }
              }
            }
          }
        }
        """;
}
