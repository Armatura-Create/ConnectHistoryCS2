using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;
using System.Threading.Tasks;
using MySqlConnector;

namespace ConnectHistory;

/// Создание и миграция схемы.
///
/// Правило: DDL идемпотентен. Плагин перезагружают на живом сервере, и повторный прогон
/// не имеет права ни упасть, ни изменить уже существующие данные.
public sealed class SchemaService
{
    /// Версия схемы. Поднимается ТОЛЬКО вместе с добавлением шага в Migrations.
    internal const int CurrentVersion = 2;

    private readonly DatabaseService _db;
    private readonly ILogger _logger;

    public SchemaService(DatabaseService db, ILogger logger)
    {
        _db = db;
        _logger = logger;
    }

    /// Полный набор DDL для чистой базы. Порядок значим только для читаемости:
    /// внешних ключей нет сознательно — они бы связали историю с агрегатами и
    /// сделали бы невозможной чистку старых сессий без каскадов.
    internal static IReadOnlyList<string> BuildSchema(string p) =>
    [
        $"""
         CREATE TABLE IF NOT EXISTS `{p}schema_version` (
           `k` VARCHAR(32) NOT NULL,
           `v` INT NOT NULL,
           `updated_at` DATETIME NOT NULL,
           PRIMARY KEY (`k`)
         ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
         """,

        $"""
         CREATE TABLE IF NOT EXISTS `{p}servers` (
           `id` INT NOT NULL,
           `address` VARCHAR(64) NOT NULL DEFAULT '',
           `hostname` VARCHAR(128) NOT NULL DEFAULT '',
           `first_seen` DATETIME NOT NULL,
           `last_seen` DATETIME NOT NULL,
           PRIMARY KEY (`id`)
         ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
         """,

        $"""
         CREATE TABLE IF NOT EXISTS `{p}players` (
           `steamid64` BIGINT UNSIGNED NOT NULL,
           `account_id` INT UNSIGNED NOT NULL,
           `first_seen` DATETIME NOT NULL,
           `last_seen` DATETIME NOT NULL,
           `sessions_count` INT UNSIGNED NOT NULL DEFAULT 0,
           `total_seconds` BIGINT UNSIGNED NOT NULL DEFAULT 0,
           `last_nickname` VARCHAR(128) NOT NULL DEFAULT '',
           `last_country` CHAR(2) NULL DEFAULT NULL,
           `last_server_id` INT NULL DEFAULT NULL,
           PRIMARY KEY (`steamid64`),
           KEY `idx_last_seen` (`last_seen`),
           KEY `idx_total_seconds` (`total_seconds`)
         ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
         """,

        $"""
         CREATE TABLE IF NOT EXISTS `{p}nicknames` (
           `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
           `steamid64` BIGINT UNSIGNED NOT NULL,
           `nickname` VARCHAR(128) NOT NULL,
           `first_seen` DATETIME NOT NULL,
           `last_seen` DATETIME NOT NULL,
           `times_seen` INT UNSIGNED NOT NULL DEFAULT 1,
           PRIMARY KEY (`id`),
           UNIQUE KEY `uq_player_nickname` (`steamid64`, `nickname`),
           KEY `idx_nickname` (`nickname`)
         ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
         """,

        // Ядро. Строка создаётся на ВХОДЕ игрока (ended_at IS NULL) и закрывается на выходе.
        // Поэтому "кто сейчас онлайн" — это ended_at IS NULL AND end_kind = 0,
        // а оборванная сессия (сервер умер) остаётся видимым фактом, а не исчезает.
        $"""
         CREATE TABLE IF NOT EXISTS `{p}sessions` (
           `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
           `session_key` CHAR(32) NOT NULL,
           `steamid64` BIGINT UNSIGNED NOT NULL,
           `account_id` INT UNSIGNED NOT NULL,
           `server_id` INT NOT NULL,
           `nickname` VARCHAR(128) NOT NULL DEFAULT '',
           `started_at` DATETIME NOT NULL,
           `ended_at` DATETIME NULL DEFAULT NULL,
           `duration_seconds` INT UNSIGNED NULL DEFAULT NULL,
           `end_kind` TINYINT UNSIGNED NOT NULL DEFAULT 0,
           `connect_map` VARCHAR(64) NOT NULL DEFAULT '',
           `disconnect_map` VARCHAR(64) NULL DEFAULT NULL,
           `disconnect_reason` SMALLINT NULL DEFAULT NULL,
           `disconnect_reason_name` VARCHAR(64) NULL DEFAULT NULL,
           `players_online` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
           `max_players` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
           `client_lang` VARCHAR(8) NULL DEFAULT NULL,
           `player_ip` VARCHAR(45) NULL DEFAULT NULL,
           `ip_hash` CHAR(64) NULL DEFAULT NULL,
           `ip_subnet` VARCHAR(45) NULL DEFAULT NULL,
           `country_iso` CHAR(2) NULL DEFAULT NULL,
           `country_name` VARCHAR(64) NULL DEFAULT NULL,
           `city` VARCHAR(128) NULL DEFAULT NULL,
           `kills` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `deaths` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `assists` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `headshots` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `damage` INT UNSIGNED NULL DEFAULT NULL,
           `mvp` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `score` INT NULL DEFAULT NULL,
           `rounds_played` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `team_final` TINYINT UNSIGNED NULL DEFAULT NULL,
           `team_changes` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `ping_avg` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `ping_min` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `ping_max` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `ping_samples` SMALLINT UNSIGNED NULL DEFAULT NULL,
           `plugin_version` VARCHAR(32) NOT NULL DEFAULT '',
           PRIMARY KEY (`id`),
           UNIQUE KEY `uq_session_key` (`session_key`),
           KEY `idx_player_time` (`steamid64`, `started_at`),
           KEY `idx_server_time` (`server_id`, `started_at`),
           KEY `idx_started_at` (`started_at`),
           KEY `idx_open` (`ended_at`)
         ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
         """,

        $"""
         CREATE TABLE IF NOT EXISTS `{p}online_snapshots` (
           `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
           `server_id` INT NOT NULL,
           `taken_at` DATETIME NOT NULL,
           `players` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
           `bots` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
           `max_players` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
           `map` VARCHAR(64) NOT NULL DEFAULT '',
           PRIMARY KEY (`id`),
           KEY `idx_server_time` (`server_id`, `taken_at`)
         ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
         """
    ];

    /// Шаги миграции для баз, созданных предыдущими версиями плагина.
    /// Ключ — версия, ДО которой поднимаемся.
    internal static IReadOnlyDictionary<int, string[]> Migrations(string prefix) =>
        new Dictionary<int, string[]>
        {
            // v2: убрать из ch_servers.address значения, которые адресом не являются.
            //
            // До этой версии плагин писал туда результат ConVar ip, а тот при обычной
            // настройке равен 0.0.0.0 — это адрес привязки сокета, а не адрес сервера.
            // Пустая строка честнее: она означает «адрес неизвестен» и, начиная с этой
            // же версии, не перезаписывается пустым значением при следующем старте
            // (см. SessionWriter.WriteServerAsync). Как только в конфиге появится
            // Server.PublicAddress, колонка заполнится настоящим адресом.
            //
            // UPDATE, а не DELETE: строка сервера нужна, испорчено только одно поле.
            [2] =
            [
                $"UPDATE `{prefix}servers` SET `address` = '' " +
                "WHERE `address` LIKE '0.0.0.0%' " +
                "   OR `address` LIKE '[::]%' " +
                "   OR `address` LIKE ':%'"
            ]
        };

    public async Task<bool> EnsureSchemaAsync(CancellationToken token)
    {
        try
        {
            await using var connection = _db.CreateConnection();
            await connection.OpenAsync(token).ConfigureAwait(false);

            foreach (var ddl in BuildSchema(_db.Prefix))
                await ExecuteAsync(connection, ddl, token).ConfigureAwait(false);

            var version = await ReadVersionAsync(connection, token).ConfigureAwait(false);

            if (version > CurrentVersion)
            {
                _logger.Error($"[DB] Схема в базе версии {version}, плагин знает только " +
                              $"{CurrentVersion}. Обновите плагин — старая версия может " +
                              "не понимать новые колонки");
                return true;
            }

            foreach (var (target, statements) in Migrations(_db.Prefix))
            {
                if (target <= version) continue;

                _logger.Info($"[DB] Миграция схемы {version} -> {target}");
                foreach (var sql in statements)
                    await ExecuteAsync(connection, sql, token).ConfigureAwait(false);

                version = target;
            }

            await WriteVersionAsync(connection, CurrentVersion, token).ConfigureAwait(false);
            _logger.Info($"[DB] Схема готова (версия {CurrentVersion.ToString(CultureInfo.InvariantCulture)})");
            return true;
        }
        catch (Exception ex)
        {
            _logger.Error("[DB] Не удалось создать/проверить схему. Плагин продолжит работу, " +
                          "но записи будут копиться в спуле до восстановления базы", ex);
            return false;
        }
    }

    /// Сессии, оставшиеся открытыми от прошлого запуска этого сервера, — это следы
    /// аварийного завершения процесса. Мы их НЕ удаляем и не выдумываем им время выхода:
    /// помечаем end_kind = Stale, чтобы «сейчас онлайн» считалось верно, а карта падений
    /// сервера осталась в данных.
    public async Task<int> MarkStaleSessionsAsync(int serverId, CancellationToken token)
    {
        try
        {
            await using var connection = _db.CreateConnection();
            await connection.OpenAsync(token).ConfigureAwait(false);

            await using var command = connection.CreateCommand();
            command.CommandTimeout = (int)_db.CommandTimeoutSeconds;
            command.CommandText =
                $"UPDATE `{_db.Prefix}sessions` SET `end_kind` = @stale " +
                "WHERE `server_id` = @server AND `ended_at` IS NULL AND `end_kind` = 0";
            command.Parameters.AddWithValue("@stale", (byte)SessionEndKind.Stale);
            command.Parameters.AddWithValue("@server", serverId);

            var affected = await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
            if (affected > 0)
                _logger.Warn($"[DB] Найдено {affected.ToString(CultureInfo.InvariantCulture)} " +
                             "незакрытых сессий от прошлого запуска — сервер завершился аварийно. " +
                             "Помечены как stale");

            return affected;
        }
        catch (Exception ex)
        {
            _logger.Error("[DB] Не удалось пометить незакрытые сессии", ex);
            return 0;
        }
    }

    private async Task ExecuteAsync(MySqlConnection connection, string sql, CancellationToken token)
    {
        await using var command = connection.CreateCommand();
        command.CommandTimeout = (int)_db.CommandTimeoutSeconds;
        command.CommandText = sql;
        await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
    }

    private async Task<int> ReadVersionAsync(MySqlConnection connection, CancellationToken token)
    {
        await using var command = connection.CreateCommand();
        command.CommandTimeout = (int)_db.CommandTimeoutSeconds;
        command.CommandText = $"SELECT `v` FROM `{_db.Prefix}schema_version` WHERE `k` = 'schema'";

        var value = await command.ExecuteScalarAsync(token).ConfigureAwait(false);
        return value is null or DBNull ? 0 : Convert.ToInt32(value, CultureInfo.InvariantCulture);
    }

    private async Task WriteVersionAsync(MySqlConnection connection, int version, CancellationToken token)
    {
        await using var command = connection.CreateCommand();
        command.CommandTimeout = (int)_db.CommandTimeoutSeconds;
        command.CommandText =
            $"INSERT INTO `{_db.Prefix}schema_version` (`k`, `v`, `updated_at`) VALUES ('schema', @v, @now) " +
            "ON DUPLICATE KEY UPDATE `v` = VALUES(`v`), `updated_at` = VALUES(`updated_at`)";
        command.Parameters.AddWithValue("@v", version);
        command.Parameters.AddWithValue("@now", DateTime.UtcNow);
        await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
    }
}
