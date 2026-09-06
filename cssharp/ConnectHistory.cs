using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Threading;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes;
using CounterStrikeSharp.API.Modules.Cvars;

namespace ConnectHistory;

// Минимум = 1.0.369: первая версия CounterStrikeSharp на .NET 10.
// Не поднимать без реальной нужды — это отсечёт сервера, на которых плагин работает.
// Значение обязано совпадать с версией пакета в .csproj (проверяет ApiVersionTests).
[MinimumApiVersion(369)]
public sealed partial class ConnectHistory : BasePlugin
{
    public override string ModuleName => "ConnectHistory";
    public override string ModuleAuthor => "Armatura";
    public override string ModuleDescription => "История подключений и статистика игроков в MySQL";

    // Версия НЕ хардкодится: берётся из метаданных сборки, которые проставляет MSBuild
    // из <Version>. Иначе её надо помнить поднять в двух местах, и релиз уезжает
    // со старым номером внутри.
    public override string ModuleVersion => PluginVersion;

    private static readonly string PluginVersion = ResolveModuleVersion(typeof(ConnectHistory).Assembly);

    internal static string ResolveModuleVersion(Assembly assembly) => FormatModuleVersion(
        assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion,
        assembly.GetName().Version);

    /// Чистая часть резолва — вынесена ради тестов.
    /// InformationalVersion может нести хвост "+Sha.abc123" от SourceLink — его отрезаем.
    internal static string FormatModuleVersion(string? informationalVersion, Version? assemblyVersion)
    {
        if (!string.IsNullOrWhiteSpace(informationalVersion))
        {
            var plus = informationalVersion.IndexOf('+', StringComparison.Ordinal);
            var trimmed = plus >= 0 ? informationalVersion[..plus] : informationalVersion;
            if (!string.IsNullOrWhiteSpace(trimmed)) return "v" + trimmed.Trim();
        }

        return assemblyVersion == null ? "v0.0.0" : "v" + assemblyVersion.ToString(3);
    }

    /// Ник обрезаем под ширину колонки: строка длиннее 128 символов иначе роняет
    /// весь INSERT с "Data too long", и сессия теряется целиком.
    internal const int NicknameMaxLength = 128;

    private ILogger _logger = null!;
    private ConfigService _configService = null!;
    private SessionService _sessions = null!;
    private GeoIpService _geoIp = null!;

    private DatabaseService? _database;
    private SessionWriter? _writer;
    private QueryService? _query;

    private CounterStrikeSharp.API.Modules.Timers.Timer? _pingTimer;
    private CounterStrikeSharp.API.Modules.Timers.Timer? _snapshotTimer;

    private readonly Dictionary<ulong, DateTime> _commandCooldown = [];

    /// Пояс, в котором время показывается игрокам. В базе всегда UTC —
    /// эта настройка только про отображение.
    private TimeZoneInfo _displayZone = TimeZoneInfo.Utc;

    public Config Config { get; private set; } = new();

    public override void Load(bool hotReload)
    {
        _logger = new PluginLogger(() => Config.Debug);
        _configService = new ConfigService(_logger);
        _sessions = new SessionService();

        ReloadConfig();
        ShowBanner();

        _geoIp = new GeoIpService(ModuleDirectory, _logger);

        BuildDatabaseStack();
        RegisterEvents();
        StartTimers();

        if (!hotReload) return;

        // Плагин загрузили на живой сервер: у игроков уже идут сессии, о которых мы
        // ничего не знаем. Время входа считаем от момента загрузки — оно занижено,
        // но это честнее, чем нулевая метка, дающая session_time около 1.7 млрд секунд.
        SafeRun("восстановление сессий после hot reload", () =>
        {
            foreach (var player in Utilities.GetPlayers())
            {
                if (player.IsBot || !player.IsValid) continue;
                OpenSessionFor(player);
            }
        });
    }

    public override void Unload(bool hotReload)
    {
        _pingTimer?.Kill();
        _snapshotTimer?.Kill();
        _pingTimer = null;
        _snapshotTimer = null;

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
    /// и из css_ch_reload — любой новый сервис, кеширующий Config, обязан оказаться здесь.
    private void BuildDatabaseStack()
    {
        _database = new DatabaseService(Config.Database, _logger);
        _database.LogTarget();

        _writer = new SessionWriter(_database, Config.Storage, ModuleDirectory, _logger);
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
            return _configService.LoadOrCreate(Application.RootDirectory);
        }
        catch (Exception ex)
        {
            _logger.Error("[Load] Конфигурацию загрузить не удалось — плагин стартует с пустыми " +
                          "настройками и писать в базу не будет. Проверьте " +
                          "configs/plugins/ConnectHistory и выполните css_ch_reload", ex);
            return new Config();
        }
    }

    /// Заставка печатается ПОСЛЕ загрузки конфига: в ней номер сервера,
    /// а он приходит оттуда.
    private void ShowBanner()
    {
        foreach (var line in Banner.Build(ModuleVersion, "CounterStrikeSharp", Config.ServerId))
            _logger.Raw(line);
    }

    private void StartTimers()
    {
        var pingInterval = Math.Max(5, Config.Collect.PingSampleIntervalSeconds);
        var snapshotInterval = Math.Max(30, Config.Collect.OnlineSnapshotIntervalSeconds);

        if (Config.Collect.Ping)
        {
            _pingTimer = AddTimer(pingInterval, () => SafeRun("замер пинга", SamplePings),
                CounterStrikeSharp.API.Modules.Timers.TimerFlags.REPEAT);
        }

        if (Config.Collect.OnlineSnapshots)
        {
            _snapshotTimer = AddTimer(snapshotInterval, () => SafeRun("снимок онлайна", TakeOnlineSnapshot),
                CounterStrikeSharp.API.Modules.Timers.TimerFlags.REPEAT);
        }

        // Регистрацию сервера откладываем: в Load() движок ещё не поднял глобальные
        // переменные, и любой ConVar.Find/Server.* падает с
        // "NativeException: Global Variables not initialized yet", а плагин не грузится вовсе.
        AddTimer(3.0f, () => SafeRun("регистрация сервера", RegisterServer));
    }

    /// Пинг снимается периодически и усредняется: единичный замер на выходе игрока
    /// ничего не говорит о качестве связи в течение сессии.
    private void SamplePings()
    {
        foreach (var player in Utilities.GetPlayers())
        {
            if (player.IsBot || !player.IsValid) continue;
            if (!_sessions.TryGet(player.SteamID, out var session)) continue;

            session.AddPing(StatsCollector.ReadPing(player));
        }
    }

    private void TakeOnlineSnapshot()
    {
        var players = 0;
        var bots = 0;

        foreach (var player in Utilities.GetPlayers())
        {
            if (player.IsBot) bots++;
            else players++;
        }

        _writer?.Enqueue(new OnlineSnapshotJob
        {
            ServerId = Config.ServerId,
            TakenAt = DateTime.UtcNow,
            Players = players,
            Bots = bots,
            MaxPlayers = Server.MaxPlayers,
            Map = CurrentMap()
        });
    }

    private void RegisterServer()
    {
        _writer?.Enqueue(new ServerUpsertJob
        {
            ServerId = Config.ServerId,
            Address = ServerAddress(),
            Hostname = ConVar.Find("hostname")?.StringValue ?? "",
            SeenAt = DateTime.UtcNow
        });
    }

    /// Адрес сервера для справочника ch_servers.
    ///
    /// Первым идёт Server.PublicAddress из конфига: публичный адрес — внешний факт,
    /// который знает владелец сервера, а не процесс. ConVar ip отдаёт адрес привязки
    /// сокета и при обычной настройке равен 0.0.0.0 — записать его означает заполнить
    /// колонку значением, по которому нельзя подключиться.
    ///
    /// Пустая строка — законный ответ «адрес неизвестен». Она НЕ затирает уже
    /// записанный адрес: за это отвечает SQL в SessionWriter.WriteServerAsync.
    ///
    /// ConVar читается ТОЛЬКО из главного потока: это натив, и обращение к нему
    /// из фонового потока убивает процесс без стека в логе.
    private string ServerAddress()
    {
        var configured = Config.Server.PublicAddress;

        string convarIp;
        int port;

        try
        {
            convarIp = ConVar.Find("ip")?.StringValue ?? "";
            port = ConVar.Find("hostport")?.GetPrimitiveValue<int>() ?? 0;
        }
        catch (Exception)
        {
            convarIp = "";
            port = 0;
        }

        var address = IpUtil.ResolvePublicAddress(configured, convarIp, port);

        if (address.Length == 0)
        {
            _logger.Warn(string.IsNullOrWhiteSpace(configured)
                ? "[Plugin] Публичный адрес сервера определить не удалось: ConVar ip = " +
                  $"\"{convarIp}\" (это адрес привязки сокета, а не адрес сервера). " +
                  "Пропишите Server.PublicAddress в Settings.json — иначе колонка address останется пустой"
                : $"[Plugin] Server.PublicAddress = \"{configured}\" не похож на публичный адрес " +
                  "(ожидается \"ip:port\" или \"host:port\", адрес не может быть 0.0.0.0 или локальным). " +
                  "Колонка address останется пустой");
        }

        return address;
    }

    internal static string CurrentMap()
    {
        try
        {
            return Server.MapName ?? "";
        }
        catch (Exception)
        {
            return "";
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
