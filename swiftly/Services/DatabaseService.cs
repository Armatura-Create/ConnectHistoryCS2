using System;
using System.Globalization;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using MySqlConnector;

namespace ConnectHistory;

/// Всё, что знает про подключение к MySQL: строка подключения, пул, проверка доступности.
/// Никаких нативов движка — сервис живёт в фоновом потоке.
public sealed partial class DatabaseService
{
    private readonly string _connectionString;
    private readonly ILogger _logger;

    /// Префикс таблиц. Уже проверен: идентификаторы в SQL нельзя параметризовать,
    /// поэтому значение из конфига обязано пройти белый список символов.
    public string Prefix { get; }

    /// Куда мы подключаемся — строка для логов БЕЗ пароля.
    public string Target { get; }

    public uint CommandTimeoutSeconds { get; }

    public DatabaseService(DatabaseConfig config, ILogger logger)
    {
        ArgumentNullException.ThrowIfNull(config);
        _logger = logger;

        Prefix = SanitizePrefix(config.TablePrefix, logger);
        _connectionString = BuildConnectionString(config);
        Target = $"{config.Host}:{config.Port.ToString(CultureInfo.InvariantCulture)}/{config.Database} " +
                 $"(user={config.User}, ssl={config.SslMode})";
        CommandTimeoutSeconds = config.CommandTimeoutSeconds == 0 ? 30 : config.CommandTimeoutSeconds;
    }

    /// Якорь \z, а не $: в .NET (как и в PCRE) $ совпадает ПЕРЕД завершающим
    /// переводом строки, поэтому префикс "ch_\n" прошёл бы белый список
    /// и уехал в текст SQL как часть имени таблицы.
    [GeneratedRegex(@"^[A-Za-z0-9_]{0,16}\z", RegexOptions.CultureInvariant)]
    private static partial Regex PrefixRegex();

    /// Префикс таблиц попадает в текст SQL как идентификатор — параметризовать его нельзя.
    /// Поэтому он проходит белый список: буквы, цифры, подчёркивание, не длиннее 16.
    /// Всё остальное — потенциальная инъекция, откатываемся на "ch_" и говорим об этом громко.
    internal static string SanitizePrefix(string? prefix, ILogger? logger = null)
    {
        if (prefix == null) return "ch_";
        if (PrefixRegex().IsMatch(prefix)) return prefix;

        logger?.Error($"[DB] TablePrefix \"{prefix}\" содержит недопустимые символы " +
                      "(разрешены латиница, цифры и '_', до 16 символов). Использую \"ch_\"");
        return "ch_";
    }

    /// Строка подключения собирается ТОЛЬКО билдером.
    ///
    /// Реальная причина: пароль от боевой базы содержит спецсимволы, а ';' в интерполяции
    /// превращается в разделитель параметров — это подмена параметров подключения
    /// (connection-string injection). Билдер экранирует значения сам.
    internal static string BuildConnectionString(DatabaseConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);

        var builder = new MySqlConnectionStringBuilder
        {
            Server = config.Host,
            Port = config.Port == 0 ? 3306u : config.Port,
            Database = config.Database,
            UserID = config.User,
            Password = config.Password,
            SslMode = ParseSslMode(config.SslMode),

            // Пул обязателен, но маленький: игровой сервер не должен держать десятки
            // соединений к базе. Писатель у нас всё равно один.
            Pooling = true,
            MinimumPoolSize = 0,
            MaximumPoolSize = config.MaxPoolSize == 0 ? 5u : config.MaxPoolSize,

            // Без таймаутов зависшая сеть превращается в вечно висящее фоновое задание.
            ConnectionTimeout = config.ConnectionTimeoutSeconds == 0 ? 10u : config.ConnectionTimeoutSeconds,
            DefaultCommandTimeout = config.CommandTimeoutSeconds == 0 ? 30u : config.CommandTimeoutSeconds,

            // Соединение могло протухнуть, пока на сервере никого не было.
            ConnectionIdleTimeout = 60,

            // MySQL хранит DATETIME без часового пояса, поэтому «в базе UTC» — это
            // соглашение, которое надо чем-то держать. DateTimeKind=Utc делает его
            // проверяемым: значение с Kind=Local драйвер писать откажется, а прочитанное
            // из базы придёт помеченным как Utc, и конвертация в пояс игрока не соврёт.
            DateTimeKind = MySqlDateTimeKind.Utc,

            CharacterSet = "utf8mb4"
        };

        return builder.ConnectionString;
    }

    /// Неизвестное значение из конфига не должно молча снижать защиту:
    /// падаем на Preferred и это видно в Target-строке лога.
    internal static MySqlSslMode ParseSslMode(string? value)
        => Enum.TryParse<MySqlSslMode>(value, ignoreCase: true, out var mode) ? mode : MySqlSslMode.Preferred;

    public MySqlConnection CreateConnection() => new(_connectionString);

    /// Проверка доступности базы. Используется командой css_ch_status и при загрузке.
    public async Task<(bool Ok, string Message)> PingAsync(CancellationToken token)
    {
        try
        {
            await using var connection = CreateConnection();
            await connection.OpenAsync(token).ConfigureAwait(false);

            await using var command = connection.CreateCommand();
            command.CommandTimeout = (int)CommandTimeoutSeconds;
            command.CommandText = "SELECT VERSION()";

            var version = await command.ExecuteScalarAsync(token).ConfigureAwait(false);
            return (true, $"MySQL {version}");
        }
        catch (Exception ex) when (ex is MySqlException or TimeoutException or InvalidOperationException)
        {
            // Текст исключения может содержать строку подключения целиком — маскируем.
            return (false, SqlSanitizer.Mask(ex.Message));
        }
    }

    public void LogTarget() => _logger.Info($"[DB] Цель: {Target}, префикс таблиц \"{Prefix}\"");
}
