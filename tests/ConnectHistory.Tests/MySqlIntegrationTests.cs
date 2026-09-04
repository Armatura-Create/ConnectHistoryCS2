using System;
using System.Threading;
using System.Threading.Tasks;
using MySqlConnector;
using Xunit;

namespace ConnectHistory.Tests;

/// Живая проверка схемы и пути записи.
///
/// Пропускается, если не задан CH_TEST_MYSQL (обычная строка подключения MySQL) —
/// в CI и на машине разработчика базы может не быть, и это не повод краснеть.
/// Поднять одноразовую базу можно так:
///   docker run --rm -d -p 3399:3306 -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=ch mysql:8
///   export CH_TEST_MYSQL="server=127.0.0.1;port=3399;user=root;password=test;database=ch"
public class MySqlIntegrationTests
{
    private static string? Raw => Environment.GetEnvironmentVariable("CH_TEST_MYSQL");

    private static DatabaseConfig? ConfigFromEnv()
    {
        if (string.IsNullOrWhiteSpace(Raw)) return null;

        var parsed = new MySqlConnectionStringBuilder(Raw);
        return new DatabaseConfig
        {
            Host = parsed.Server,
            Port = parsed.Port,
            Database = parsed.Database,
            User = parsed.UserID,
            Password = parsed.Password,
            SslMode = "None",
            TablePrefix = "cht_"
        };
    }

    [SkippableFact]
    public async Task SchemaIsCreatedAndSurvivesASecondRun()
    {
        var config = ConfigFromEnv();
        Skip.If(config == null, "CH_TEST_MYSQL не задан");

        var logger = new NullLogger();
        var db = new DatabaseService(config!, logger);
        var schema = new SchemaService(db, logger);

        Assert.True(await schema.EnsureSchemaAsync(CancellationToken.None));
        // Плагин перезагружают на живом сервере — повторный прогон DDL обязан пройти
        Assert.True(await schema.EnsureSchemaAsync(CancellationToken.None));
    }

    [SkippableFact]
    public async Task SessionIsOpenedThenClosed_AndAggregatesCountItOnce()
    {
        var config = ConfigFromEnv();
        Skip.If(config == null, "CH_TEST_MYSQL не задан");

        var logger = new NullLogger();
        var db = new DatabaseService(config!, logger);
        var schema = new SchemaService(db, logger);
        Assert.True(await schema.EnsureSchemaAsync(CancellationToken.None));

        var steamId = 76561198000000000UL + (ulong)Random.Shared.Next(1, 1_000_000);
        var key = Guid.NewGuid().ToString("N");
        var started = DateTime.UtcNow.AddMinutes(-30);

        var storage = new StorageConfig { SpoolEnabled = false, RetryAttempts = 1 };
        var spoolDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        System.IO.Directory.CreateDirectory(spoolDir);

        var writer = new SessionWriter(db, storage, spoolDir, logger);

        writer.Enqueue(new SessionOpenJob
        {
            SessionKey = key,
            SteamId64 = steamId,
            AccountId = SteamIdUtil.ToAccountId(steamId),
            ServerId = 99,
            Nickname = "Тестовый Игрок",
            StartedAt = started,
            ConnectMap = "de_dust2",
            PlayersOnline = 7,
            MaxPlayers = 64,
            PlayerIp = "203.0.113.77",
            IpHash = IpUtil.Hash("203.0.113.77", "salt"),
            IpSubnet = IpUtil.ToSubnet("203.0.113.77"),
            CountryIso = "DE",
            PluginVersion = "v2.0.0"
        });

        var close = new SessionCloseJob
        {
            SessionKey = key,
            SteamId64 = steamId,
            AccountId = SteamIdUtil.ToAccountId(steamId),
            ServerId = 99,
            Nickname = "Тестовый Игрок",
            StartedAt = started,
            EndedAt = started.AddMinutes(30),
            DurationSeconds = 1800,
            DisconnectMap = "de_dust2",
            DisconnectReason = 2,
            DisconnectReasonName = "DISCONNECT_BY_USER",
            EndKind = SessionEndKind.Disconnect,
            Stats = new MatchStatsSnapshot { Kills = 10, Deaths = 4, Assists = 2, Damage = 1500, RoundsPlayed = 12, Team = 3 },
            PingAvg = 42, PingMin = 30, PingMax = 55, PingSamples = 6
        };

        writer.Enqueue(close);

        // Повтор того же закрытия имитирует досылку из спула: счётчики не должны задвоиться
        writer.Enqueue(close);

        await writer.DisposeAsync();

        await using var connection = db.CreateConnection();
        await connection.OpenAsync();

        await using (var command = connection.CreateCommand())
        {
            command.CommandText = "SELECT `duration_seconds`, `kills`, `end_kind`, `country_iso`, `ping_avg` " +
                                  "FROM `cht_sessions` WHERE `session_key` = @key";
            command.Parameters.AddWithValue("@key", key);

            await using var reader = await command.ExecuteReaderAsync();
            Assert.True(await reader.ReadAsync(), "строка сессии не найдена");
            Assert.Equal(1800, reader.GetInt32(0));
            Assert.Equal(10, reader.GetInt32(1));
            Assert.Equal((byte)SessionEndKind.Disconnect, reader.GetByte(2));
            Assert.Equal("DE", reader.GetString(3));
            Assert.Equal(42, reader.GetInt32(4));
        }

        await using (var command = connection.CreateCommand())
        {
            command.CommandText = "SELECT `sessions_count`, `total_seconds` FROM `cht_players` WHERE `steamid64` = @steam";
            command.Parameters.AddWithValue("@steam", steamId);

            await using var reader = await command.ExecuteReaderAsync();
            Assert.True(await reader.ReadAsync(), "агрегат игрока не создан");
            Assert.Equal(1u, reader.GetUInt32(0));      // ровно одна сессия, несмотря на дубль
            Assert.Equal(1800UL, reader.GetUInt64(1));
        }

        await using (var command = connection.CreateCommand())
        {
            command.CommandText = "SELECT `times_seen` FROM `cht_nicknames` WHERE `steamid64` = @steam";
            command.Parameters.AddWithValue("@steam", steamId);
            Assert.Equal(1u, Convert.ToUInt32(await command.ExecuteScalarAsync(), System.Globalization.CultureInfo.InvariantCulture));
        }

        System.IO.Directory.Delete(spoolDir, recursive: true);
    }

    [SkippableFact]
    public async Task CloseWithoutOpen_StillLandsAsACompleteRow()
    {
        // Задание на открытие могло потеряться (спул обрезали, база лежала дольше ретраев).
        // Закрытие обязано вставить полную строку, а не молча пропасть.
        var config = ConfigFromEnv();
        Skip.If(config == null, "CH_TEST_MYSQL не задан");

        var logger = new NullLogger();
        var db = new DatabaseService(config!, logger);
        var schema = new SchemaService(db, logger);
        Assert.True(await schema.EnsureSchemaAsync(CancellationToken.None));

        var steamId = 76561198000000000UL + (ulong)Random.Shared.Next(1, 1_000_000);
        var key = Guid.NewGuid().ToString("N");
        var started = DateTime.UtcNow.AddMinutes(-10);

        var spoolDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        System.IO.Directory.CreateDirectory(spoolDir);

        var writer = new SessionWriter(db, new StorageConfig { SpoolEnabled = false, RetryAttempts = 1 }, spoolDir, logger);
        writer.Enqueue(new SessionCloseJob
        {
            SessionKey = key,
            SteamId64 = steamId,
            AccountId = SteamIdUtil.ToAccountId(steamId),
            ServerId = 55,
            Nickname = "Orphan",
            StartedAt = started,
            EndedAt = started.AddMinutes(10),
            DurationSeconds = 600,
            DisconnectMap = "de_nuke",
            DisconnectReason = 4,
            DisconnectReasonName = "LOST",
            EndKind = SessionEndKind.Disconnect
        });
        await writer.DisposeAsync();

        await using var connection = db.CreateConnection();
        await connection.OpenAsync();
        await using var command = connection.CreateCommand();
        command.CommandText = "SELECT `duration_seconds`, `nickname`, `server_id` FROM `cht_sessions` WHERE `session_key` = @key";
        command.Parameters.AddWithValue("@key", key);

        await using var reader = await command.ExecuteReaderAsync();
        Assert.True(await reader.ReadAsync(), "закрытие без открытия потеряло сессию");
        Assert.Equal(600, reader.GetInt32(0));
        Assert.Equal("Orphan", reader.GetString(1));
        Assert.Equal(55, reader.GetInt32(2));

        System.IO.Directory.Delete(spoolDir, recursive: true);
    }

    [SkippableFact]
    public async Task TimestampsAreStoredAndReadBackAsUtc()
    {
        var config = ConfigFromEnv();
        Skip.If(config == null, "CH_TEST_MYSQL не задан");

        var logger = new NullLogger();
        var db = new DatabaseService(config!, logger);
        var schema = new SchemaService(db, logger);
        Assert.True(await schema.EnsureSchemaAsync(CancellationToken.None));

        var steamId = 76561198000000000UL + (ulong)Random.Shared.Next(1, 1_000_000);
        var key = Guid.NewGuid().ToString("N");
        var startedUtc = new DateTime(2026, 9, 4, 21, 30, 0, DateTimeKind.Utc);

        var spoolDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        System.IO.Directory.CreateDirectory(spoolDir);

        var writer = new SessionWriter(db, new StorageConfig { SpoolEnabled = false, RetryAttempts = 1 }, spoolDir, logger);
        writer.Enqueue(new SessionOpenJob
        {
            SessionKey = key,
            SteamId64 = steamId,
            AccountId = SteamIdUtil.ToAccountId(steamId),
            ServerId = 42,
            Nickname = "UtcCheck",
            StartedAt = startedUtc,
            ConnectMap = "de_ancient"
        });
        await writer.DisposeAsync();

        await using var connection = db.CreateConnection();
        await connection.OpenAsync();

        // 1. В самой колонке лежат ровно те же цифры, что мы отправили: MySQL не двигает
        //    DATETIME, а значит в базе именно UTC, а не время машины сервера.
        await using (var raw = connection.CreateCommand())
        {
            raw.CommandText = "SELECT DATE_FORMAT(`started_at`, '%Y-%m-%d %H:%i:%s') FROM `cht_sessions` WHERE `session_key` = @key";
            raw.Parameters.AddWithValue("@key", key);
            Assert.Equal("2026-09-04 21:30:00", (string?)await raw.ExecuteScalarAsync());
        }

        // 2. Прочитанное значение помечено как UTC — конвертация в пояс игрока не соврёт
        await using (var typed = connection.CreateCommand())
        {
            typed.CommandText = "SELECT `started_at` FROM `cht_sessions` WHERE `session_key` = @key";
            typed.Parameters.AddWithValue("@key", key);

            await using var reader = await typed.ExecuteReaderAsync();
            Assert.True(await reader.ReadAsync());

            var readBack = reader.GetDateTime(0);
            Assert.Equal(DateTimeKind.Utc, readBack.Kind);
            Assert.Equal(startedUtc, readBack);
            Assert.Equal("2026-09-05 00:30", ChatFormat.Date(readBack, TimeZoneResolver.Resolve("Europe/Moscow")));
        }

        // 3. Локальное время драйвер писать откажется — соглашение «в базе UTC»
        //    держится не комментарием, а строкой подключения
        await using (var local = connection.CreateCommand())
        {
            local.CommandText = "UPDATE `cht_sessions` SET `ended_at` = @ended WHERE `session_key` = @key";
            local.Parameters.AddWithValue("@ended", new DateTime(2026, 9, 4, 21, 30, 0, DateTimeKind.Local));
            local.Parameters.AddWithValue("@key", key);

            await Assert.ThrowsAnyAsync<Exception>(() => local.ExecuteNonQueryAsync());
        }

        System.IO.Directory.Delete(spoolDir, recursive: true);
    }

    [SkippableFact]
    public async Task StaleSessionsAreMarked_NotDeleted()
    {
        var config = ConfigFromEnv();
        Skip.If(config == null, "CH_TEST_MYSQL не задан");

        var logger = new NullLogger();
        var db = new DatabaseService(config!, logger);
        var schema = new SchemaService(db, logger);
        Assert.True(await schema.EnsureSchemaAsync(CancellationToken.None));

        var steamId = 76561198000000000UL + (ulong)Random.Shared.Next(1, 1_000_000);
        var key = Guid.NewGuid().ToString("N");

        var storage = new StorageConfig { SpoolEnabled = false, RetryAttempts = 1 };
        var spoolDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        System.IO.Directory.CreateDirectory(spoolDir);

        var writer = new SessionWriter(db, storage, spoolDir, logger);
        writer.Enqueue(new SessionOpenJob
        {
            SessionKey = key,
            SteamId64 = steamId,
            AccountId = SteamIdUtil.ToAccountId(steamId),
            ServerId = 77,
            Nickname = "Crashed",
            StartedAt = DateTime.UtcNow.AddHours(-2),
            ConnectMap = "de_mirage"
        });
        await writer.DisposeAsync();

        // Сервер «упал»: сессия осталась открытой, следующий старт помечает её stale
        Assert.True(await schema.MarkStaleSessionsAsync(77, CancellationToken.None) > 0);

        await using var connection = db.CreateConnection();
        await connection.OpenAsync();
        await using var command = connection.CreateCommand();
        command.CommandText = "SELECT `end_kind`, `ended_at` IS NULL FROM `cht_sessions` WHERE `session_key` = @key";
        command.Parameters.AddWithValue("@key", key);

        await using var reader = await command.ExecuteReaderAsync();
        Assert.True(await reader.ReadAsync(), "оборванная сессия исчезла — а она факт, а не мусор");
        Assert.Equal((byte)SessionEndKind.Stale, reader.GetByte(0));
        Assert.True(reader.GetBoolean(1));   // время выхода не выдумываем

        System.IO.Directory.Delete(spoolDir, recursive: true);
    }
}
