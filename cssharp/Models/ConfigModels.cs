using System.Collections.Generic;

namespace ConnectHistory;

/// Модель Settings.json.
/// Каждое поле nullable там, где отсутствие значения должно означать «взять дефолт»:
/// битый или частичный файл не должен приводить к нулям в БД.
public sealed class SettingsConfig
{
    public bool Debug { get; set; }

    /// Идентификатор сервера в таблице ch_servers, ДО версии 3.0.1.
    ///
    /// Переехал в Server.Id. Читается по-прежнему, потому что молча сбросить номер
    /// в 1 на обновлении означает слить истории двух серверов в одну, и заметят
    /// это только по кривому отчёту. При наличии обоих выигрывает Server.Id.
    public int? ServerId { get; set; }

    /// Язык по умолчанию для сообщений игроцких команд.
    public string DefaultLang { get; set; } = "RU";

    /// Часовой пояс, в котором время показывается ИГРОКАМ: IANA-идентификатор
    /// ("Europe/Moscow"), "UTC" или "Local" (пояс машины сервера).
    /// На то, что пишется в базу, не влияет — там всегда UTC.
    public string DisplayTimeZone { get; set; } = "UTC";

    public ServerConfig? Server { get; set; }
    public DatabaseConfig? Database { get; set; }
    public CollectConfig? Collect { get; set; }
    public StorageConfig? Storage { get; set; }
    public CommandsConfig? Commands { get; set; }
}

/// Как сервер представляется в справочнике ch_servers.
public sealed class ServerConfig
{
    /// Идентификатор сервера в таблице ch_servers. Разные сервера — разные значения.
    /// Null означает «в файле не задано»: тогда берётся устаревший ServerId из корня,
    /// а если нет и его — 1.
    public int? Id { get; set; }

    /// Публичный адрес сервера в виде "ip:port" или "host:port".
    ///
    /// Зачем это настройка, а не автоопределение: процесс игрового сервера НЕ ЗНАЕТ
    /// своего публичного адреса и знать не может. ConVar ip отдаёт адрес привязки
    /// сокета, и при обычной конфигурации это 0.0.0.0 — «слушаю все интерфейсы».
    /// Записать 0.0.0.0 как адрес сервера означает записать заведомо бесполезное
    /// значение: по нему нельзя подключиться и нельзя отличить один сервер от другого.
    ///
    /// Пусто — плагин попробует прочитать ConVar и отбракует результат, если тот
    /// окажется адресом привязки, локальным или приватным. Тогда колонка address
    /// останется пустой, а не заполнится мусором.
    public string PublicAddress { get; set; } = "";
}

/// Параметры подключения к MySQL.
/// Строка подключения НИКОГДА не собирается интерполяцией — только
/// MySqlConnectionStringBuilder (см. DatabaseService): пароль с ';' иначе
/// подменяет параметры подключения.
public sealed class DatabaseConfig
{
    public string Host { get; set; } = "127.0.0.1";
    public uint Port { get; set; } = 3306;
    public string Database { get; set; } = "connect_history";
    public string User { get; set; } = "";
    public string Password { get; set; } = "";

    /// None | Preferred | Required | VerifyCA | VerifyFull.
    /// Required — разумный минимум для БД за пределами localhost: без него
    /// трафик с ником, IP и SteamID идёт открытым текстом.
    public string SslMode { get; set; } = "Preferred";

    /// Префикс таблиц. Позволяет держать несколько проектов в одной базе.
    public string TablePrefix { get; set; } = "ch_";

    /// Пул держим маленьким: игровой сервер не должен занимать десятки коннектов.
    public uint MaxPoolSize { get; set; } = 5;
    public uint ConnectionTimeoutSeconds { get; set; } = 10;
    public uint CommandTimeoutSeconds { get; set; } = 30;
}

/// Что именно собирать. Каждый уровень персональных данных — отдельный флаг.
public sealed class CollectConfig
{
    /// Страна и город по GeoLite2. Требует .mmdb рядом с плагином.
    public bool GeoIp { get; set; } = true;

    /// Полный IP игрока. Персональные данные — отключается независимо от гео.
    public bool PlayerIp { get; set; } = true;

    /// HMAC-SHA256 от IP: позволяет искать мультиаккаунты, не храня сам адрес.
    public bool IpHash { get; set; } = true;

    /// Соль для IpHash. Пустая соль = хеш обратим перебором IPv4 за минуты,
    /// поэтому при пустом значении хеширование выключается с предупреждением.
    public string IpHashSalt { get; set; } = "";

    /// Итоги матча: убийства, смерти, урон, MVP, счёт.
    public bool MatchStats { get; set; } = true;

    /// Средний/минимальный/максимальный пинг за сессию.
    public bool Ping { get; set; } = true;

    /// Считать ли в «наиграно» время, проведённое наблюдателем и без команды.
    ///
    /// true (по умолчанию) — прежнее поведение: наиграно = время подключения.
    /// false — из ch_players.total_seconds вычитается время вне игры, поэтому
    /// висящий в спектаторах всю ночь не попадает в топ по наигранному.
    ///
    /// На ch_sessions.duration_seconds настройка НЕ влияет: там всегда честное
    /// время подключения. Время вне игры пишется в spectator_seconds всегда,
    /// независимо от настройки, — передумав, можно пересчитать агрегат.
    public bool CountSpectatorTime { get; set; } = true;

    /// Таблица ch_nicknames — история смены ников.
    public bool NicknameHistory { get; set; } = true;

    /// Таблица ch_players — агрегаты (наиграно всего, число сессий, first/last seen).
    public bool PlayerAggregates { get; set; } = true;

    /// Таблица ch_online_snapshots — точки для графика посещаемости.
    public bool OnlineSnapshots { get; set; } = true;

    public int OnlineSnapshotIntervalSeconds { get; set; } = 300;

    /// Как часто снимать пинг игроков (главный поток, дешёвая операция).
    public int PingSampleIntervalSeconds { get; set; } = 30;
}

/// Поведение при недоступной БД.
public sealed class StorageConfig
{
    /// Сбрасывать неотправленные записи в файл и досылать при следующем старте.
    /// Без этого падение БД на 10 минут = потерянные сессии.
    public bool SpoolEnabled { get; set; } = true;

    /// Верхняя граница файла спула, чтобы он не съел диск на сервере.
    public int SpoolMaxEntries { get; set; } = 20000;

    public int RetryAttempts { get; set; } = 3;
    public int RetryDelaySeconds { get; set; } = 5;
}

/// Команды в игре.
public sealed class CommandsConfig
{
    /// css_playtime / css_lastseen для обычных игроков.
    public bool PlayerCommandsEnabled { get; set; } = true;

    /// Кулдаун на игрока: команды ходят в БД.
    public int CooldownSeconds { get; set; } = 10;

    /// Сколько последних сессий показывает css_lastseen.
    public int LastSeenLimit { get; set; } = 5;
}

/// Модель Messages.json: ключ -> язык -> текст.
public sealed class MessagesConfig
{
    public Dictionary<string, Dictionary<string, string>>? Messages { get; set; }
}

/// Склеенная конфигурация, с которой работает плагин.
public sealed class Config
{
    public bool Debug { get; set; }
    public int ServerId { get; set; } = 1;
    public string DefaultLang { get; set; } = "RU";
    public string DisplayTimeZone { get; set; } = "UTC";
    public ServerConfig Server { get; set; } = new();
    public DatabaseConfig Database { get; set; } = new();
    public CollectConfig Collect { get; set; } = new();
    public StorageConfig Storage { get; set; } = new();
    public CommandsConfig Commands { get; set; } = new();
    public Dictionary<string, Dictionary<string, string>> Messages { get; set; } = new();
}
