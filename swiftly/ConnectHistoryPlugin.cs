using System;
using System.Collections.Generic;
using System.Threading;
using SwiftlyS2.Shared;
using SwiftlyS2.Shared.Plugins;

namespace ConnectHistory;

/// Плагин под SwiftlyS2.
///
/// SwiftlyS2 — не надстройка над Metamod, а альтернативный лоадер: он подключается
/// строкой `Game csgo/addons/swiftlys2` в gameinfo.gi и Metamod не требует.
/// Поэтому это отдельная реализация, а не порт-обёртка над версией для CSSharp:
/// код продублирован сознательно, чтобы цели не тянули друг друга.
///
/// MinimumAPIVersion держим равным версии пакета, против которого собираемся
/// (ApiVersionTests это проверяет): собираться против МИНИМАЛЬНОЙ поддерживаемой
/// версии — единственный способ дать компилятору доказать, что API из более свежих
/// сборок не используется.
[PluginMetadata(
    Id = "ConnectHistory",
    Name = "ConnectHistory",
    Version = GeneratedVersion.Value,
    Author = "Armatura",
    Description = "История подключений и статистика игроков в MySQL",
    Website = "https://github.com/Armatura-Create/ConnectHistoryCS2",
    MinimumAPIVersion = SwiftlyApiVersion)]
[System.Diagnostics.CodeAnalysis.SuppressMessage("Design", "CA1001",
    Justification = "Жизненный цикл задаёт SwiftlyS2: ресурсы освобождает Unload, " +
                    "который фреймворк вызывает сам. IDisposable он не вызывает, " +
                    "и Dispose, которого никто не зовёт, только маскировал бы " +
                    "настоящую точку освобождения.")]
public sealed partial class ConnectHistory : BasePlugin
{
    /// Обязана совпадать с версией пакета SwiftlyS2.CS2 в .csproj.
    internal const string SwiftlyApiVersion = "1.4.9";

    // Версия для логов и sw_ch_status берётся из метаданных сборки — тот же источник,
    // что и GeneratedVersion, но с префиксом "v" и без хвоста SourceLink.
    private static readonly string PluginVersion = PluginText.ResolveModuleVersion(typeof(PluginText).Assembly);

    private ILogger _logger = null!;
    private ConfigService _configService = null!;
    private SessionService _sessions = null!;
    private GeoIpService _geoIp = null!;

    private DatabaseService? _database;
    private SessionWriter? _writer;
    private QueryService? _query;

    // Таймеры SwiftlyS2 отменяются через CancellationTokenSource, отданный планировщиком.
    private CancellationTokenSource? _pingTimer;
    private CancellationTokenSource? _snapshotTimer;
    private CancellationTokenSource? _registerTimer;

    private readonly Dictionary<ulong, DateTime> _commandCooldown = [];

    /// Пояс, в котором время показывается игрокам. В базе всегда UTC —
    /// эта настройка только про отображение.
    private TimeZoneInfo _displayZone = TimeZoneInfo.Utc;

    public Config Config { get; private set; } = new();

    public ConnectHistory(ISwiftlyCore core) : base(core)
    {
    }

    public override void Load(bool hotReload)
    {
        // Логгер плагина пишет через Core.Logger: свой Console.WriteLine потерял бы
        // и метку плагина, и маршрутизацию в файловые синки SwiftlyS2.
        _logger = new PluginLogger(Core.Logger, () => Config.Debug);
        _configService = new ConfigService(_logger);
        _sessions = new SessionService();

        ReloadConfig();

        // Базы GeoLite2 лежат рядом с DLL плагина, а спул неотправленных записей —
        // в data-каталоге: каталог плагина перезаписывается при обновлении,
        // и спул вместе с ним потерялся бы.
        _geoIp = new GeoIpService(Core.PluginPath, _logger);

        BuildDatabaseStack();
        RegisterEvents();
        RegisterCommands();
        StartTimers();

        if (!hotReload) return;

        // Плагин загрузили на живой сервер: у игроков уже идут сессии, о которых мы
        // ничего не знаем. Время входа считаем от момента загрузки — оно занижено,
        // но это честнее, чем нулевая метка, дающая session_time около 1.7 млрд секунд.
        SafeRun("восстановление сессий после hot reload", OpenSessionsForEveryone);
    }

    public override void Unload()
    {
        StopTimers();
        UnregisterEvents();
        UnregisterCommands();

        // Открытые сессии закрываем явно: иначе выгрузка плагина оставляет их
        // висеть в базе, и «кто сейчас онлайн» врёт до следующего старта.
        CloseAllSessions(SessionEndKind.PluginUnload);

        var writer = _writer;
        _writer = null;

        if (writer != null)
        {
            // Дожидаемся слива очереди: внутри стоит собственный таймаут в 5 секунд,
            // остаток уходит в спул. Выгрузка плагина не должна вешать сервер.
            try
            {
                writer.DisposeAsync().AsTask().GetAwaiter().GetResult();
            }
            catch (Exception ex)
            {
                _logger.Error("[Unload] Не удалось корректно остановить писателя", ex);
            }
        }

        _geoIp?.Dispose();
        _commandCooldown.Clear();
    }

    /// Пересоздаёт всё, что зависит от конфигурации базы. Вызывается на загрузке
    /// и из ch_reload — любой новый сервис, кеширующий Config, обязан оказаться здесь.
    private void BuildDatabaseStack()
    {
        _database = new DatabaseService(Config.Database, _logger);
        _database.LogTarget();

        _writer = new SessionWriter(_database, Config.Storage, Core.PluginDataDirectory, _logger);
        _query = new QueryService(_database, _logger);

        var database = _database;
        var serverId = Config.ServerId;

        // Схема создаётся в фоне: DDL по сети может занять секунды, а Load() держит
        // загрузку сервера.
        _ = System.Threading.Tasks.Task.Run(async () =>
        {
            var schema = new SchemaService(database, _logger);
            if (await schema.EnsureSchemaAsync(CancellationToken.None).ConfigureAwait(false))
                await schema.MarkStaleSessionsAsync(serverId, CancellationToken.None).ConfigureAwait(false);
        });
    }

    /// Единственная точка применения конфигурации: всё, что из неё выводится
    /// (например часовой пояс отображения), пересчитывается здесь и нигде больше.
    private void ReloadConfig()
    {
        Config = LoadConfigSafely();
        _displayZone = TimeZoneResolver.Resolve(Config.DisplayTimeZone, _logger);
    }

    private Config LoadConfigSafely()
    {
        try
        {
            // Core.Configuration.BasePath — это уже configs/plugins/ConnectHistory,
            // каталог целиком, а не корень сервера.
            return _configService.LoadOrCreate(Core.Configuration.BasePath);
        }
        catch (Exception ex)
        {
            _logger.Error("[Load] Конфигурацию загрузить не удалось — плагин стартует с пустыми " +
                          "настройками и писать в базу не будет. Проверьте " +
                          "configs/plugins/ConnectHistory и выполните sw_ch_reload", ex);
            return new Config();
        }
    }

    private void StartTimers()
    {
        var pingInterval = Math.Max(5, Config.Collect.PingSampleIntervalSeconds);
        var snapshotInterval = Math.Max(30, Config.Collect.OnlineSnapshotIntervalSeconds);

        // DelayAndRepeatBySeconds, а не RepeatBySeconds: последний выполняет задачу
        // сразу же, а нам первый замер нужен не раньше, чем через интервал.
        if (Config.Collect.Ping)
        {
            _pingTimer = Core.Scheduler.DelayAndRepeatBySeconds(pingInterval, pingInterval,
                () => SafeRun("замер пинга", SamplePings));
        }

        if (Config.Collect.OnlineSnapshots)
        {
            _snapshotTimer = Core.Scheduler.DelayAndRepeatBySeconds(snapshotInterval, snapshotInterval,
                () => SafeRun("снимок онлайна", TakeOnlineSnapshot));
        }

        // Регистрацию сервера откладываем: на момент Load() движок ещё не обязан
        // отдать ConVar и глобальные переменные, а ошибка здесь стоит загрузки плагина.
        _registerTimer = Core.Scheduler.DelayBySeconds(3.0f, () => SafeRun("регистрация сервера", RegisterServer));
    }

    private void StopTimers()
    {
        _pingTimer?.Cancel();
        _snapshotTimer?.Cancel();
        _registerTimer?.Cancel();
        _pingTimer = null;
        _snapshotTimer = null;
        _registerTimer = null;
    }

    /// Пинг снимается периодически и усредняется: единичный замер на выходе игрока
    /// ничего не говорит о качестве связи в течение сессии.
    private void SamplePings()
    {
        foreach (var player in Core.PlayerManager.GetAllPlayers())
        {
            if (player.IsFakeClient) continue;
            if (!_sessions.TryGet(player.SteamID, out var session)) continue;

            session.AddPing(StatsCollector.ReadPing(player));
        }
    }

    private void TakeOnlineSnapshot()
    {
        var players = 0;
        var bots = 0;

        foreach (var player in Core.PlayerManager.GetAllPlayers())
        {
            if (player.IsFakeClient) bots++;
            else players++;
        }

        _writer?.Enqueue(new OnlineSnapshotJob
        {
            ServerId = Config.ServerId,
            TakenAt = DateTime.UtcNow,
            Players = players,
            Bots = bots,
            MaxPlayers = SafeMaxPlayers(),
            Map = CurrentMap()
        });
    }

    private void RegisterServer()
    {
        _writer?.Enqueue(new ServerUpsertJob
        {
            ServerId = Config.ServerId,
            Address = ServerAddress(),
            Hostname = ReadConVar("hostname"),
            SeenAt = DateTime.UtcNow
        });
    }

    /// Адрес сервера для справочника ch_servers.
    ///
    /// Первым идёт Server.PublicAddress из конфига: публичный адрес — внешний факт,
    /// который знает владелец сервера, а не процесс. Автоопределение (Core.Engine.ServerIP
    /// плюс hostport) оставлено запасным путём и отбраковывается на адресе привязки,
    /// loopback и приватных сетях — записать 0.0.0.0 означает заполнить колонку
    /// значением, по которому нельзя подключиться.
    ///
    /// Пустая строка — законный ответ «адрес неизвестен». Она НЕ затирает уже
    /// записанный адрес: за это отвечает SQL в SessionWriter.
    private string ServerAddress()
    {
        var configured = Config.Server.PublicAddress;

        string engineIp;
        int port;

        try
        {
            engineIp = Core.Engine.ServerIP ?? "";
            port = int.TryParse(ReadConVar("hostport"), System.Globalization.NumberStyles.Integer,
                System.Globalization.CultureInfo.InvariantCulture, out var parsed) ? parsed : 0;
        }
        catch (Exception)
        {
            engineIp = "";
            port = 0;
        }

        var address = IpUtil.ResolvePublicAddress(configured, engineIp, port);

        if (address.Length == 0)
        {
            _logger.Warn(string.IsNullOrWhiteSpace(configured)
                ? "[Plugin] Публичный адрес сервера определить не удалось: движок отдал " +
                  $"\"{engineIp}\" (это адрес привязки сокета, а не адрес сервера). " +
                  "Пропишите Server.PublicAddress в Settings.json — иначе колонка address останется пустой"
                : $"[Plugin] Server.PublicAddress = \"{configured}\" не похож на публичный адрес " +
                  "(ожидается \"ip:port\" или \"host:port\", адрес не может быть 0.0.0.0 или локальным). " +
                  "Колонка address останется пустой");
        }

        return address;
    }

    /// ConVar читаются ТОЛЬКО из главного потока: это нативы, и обращение к ним
    /// из фонового потока убивает процесс без стека в логе.
    private string ReadConVar(string name)
    {
        try
        {
            return Core.ConVar.FindAsString(name)?.ValueAsString ?? "";
        }
        catch (Exception)
        {
            return "";
        }
    }

    private string CurrentMap()
    {
        try
        {
            return Core.Engine.GlobalVars.MapName.Value ?? "";
        }
        catch (Exception)
        {
            return "";
        }
    }

    private int SafeMaxPlayers()
    {
        try
        {
            return Core.PlayerManager.MaxPlayers;
        }
        catch (Exception)
        {
            return 0;
        }
    }

    /// Запускает необязательное действие, не давая его падению сорвать работу плагина.
    private void SafeRun(string what, Action action)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            _logger.Error($"[Plugin] Ошибка: {what}", ex);
        }
    }
}
