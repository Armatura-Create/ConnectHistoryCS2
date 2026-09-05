using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using MySqlConnector;

namespace ConnectHistory;

/// Единственное место, где плагин пишет в базу.
///
/// Главный поток кладёт готовый снимок в канал и немедленно возвращается в кадр —
/// ни одно обращение к нативам движка сюда не попадает и попасть не может: через границу
/// проходят только POCO-копии значений.
///
/// Писатель один. Не «по задаче на игрока»: 64 игрока на смене карты — это 64 параллельных
/// подключения к MySQL из игрового процесса.
public sealed class SessionWriter : IAsyncDisposable
{
    private readonly DatabaseService _db;
    private readonly ILogger _logger;
    private readonly StorageConfig _storage;
    private readonly string _spoolPath;

    private readonly Channel<WriteJob> _channel = Channel.CreateUnbounded<WriteJob>(
        new UnboundedChannelOptions { SingleReader = true, SingleWriter = false });

    private readonly CancellationTokenSource _cts = new();
    private readonly Task _loop;

    private static readonly JsonSerializerOptions SpoolOptions = new() { WriteIndented = false };

    private int _pending;
    private long _written;
    private long _spooled;
    private string? _lastError;
    private DateTime _lastSpoolReplay = DateTime.MinValue;

    public SessionWriter(DatabaseService db, StorageConfig storage, string spoolDirectory, ILogger logger)
    {
        _db = db;
        _storage = storage;
        _logger = logger;
        _spoolPath = Path.Combine(spoolDirectory, "pending-writes.jsonl");
        _loop = Task.Run(() => RunAsync(_cts.Token));
    }

    // ---- Наблюдаемое состояние (для css_ch_status) -----------------------------

    public int Pending => Volatile.Read(ref _pending);
    public long Written => Interlocked.Read(ref _written);
    public long Spooled => Interlocked.Read(ref _spooled);
    public string? LastError => Volatile.Read(ref _lastError);
    public bool SpoolExists => File.Exists(_spoolPath);

    // ---- Приём заданий ---------------------------------------------------------

    /// Вызывается из главного потока. Никогда не блокирует и никогда не бросает:
    /// проблема с базой не имеет права сорвать обработчик игрового события.
    public void Enqueue(WriteJob job)
    {
        ArgumentNullException.ThrowIfNull(job);

        if (_channel.Writer.TryWrite(job))
        {
            Interlocked.Increment(ref _pending);
            return;
        }

        _logger.Error($"[DB] Очередь записи закрыта, задание потеряно: {job.Describe}");
    }

    // ---- Фоновый цикл ----------------------------------------------------------

    private async Task RunAsync(CancellationToken token)
    {
        // Спул с прошлого запуска досылаем первым делом: сервер мог упасть с непустой очередью.
        await ReplaySpoolAsync(token).ConfigureAwait(false);

        try
        {
            await foreach (var job in _channel.Reader.ReadAllAsync(token).ConfigureAwait(false))
            {
                await ProcessAsync(job, token).ConfigureAwait(false);
                Interlocked.Decrement(ref _pending);
            }
        }
        catch (OperationCanceledException)
        {
            // Штатная остановка — оставшееся сбросим в спул в DisposeAsync
        }
        catch (Exception ex)
        {
            _logger.Error("[DB] Фоновый писатель остановлен из-за необработанной ошибки", ex);
        }
    }

    private async Task ProcessAsync(WriteJob job, CancellationToken token)
    {
        var attempts = Math.Max(1, _storage.RetryAttempts);
        var delay = TimeSpan.FromSeconds(Math.Max(1, _storage.RetryDelaySeconds));

        for (var attempt = 1; attempt <= attempts; attempt++)
        {
            job.Attempts = attempt;

            try
            {
                await WriteAsync(job, token).ConfigureAwait(false);
                Interlocked.Increment(ref _written);
                Volatile.Write(ref _lastError, null);

                await TryReplaySpoolAsync(token).ConfigureAwait(false);
                return;
            }
            catch (OperationCanceledException)
            {
                Spool(job);
                return;
            }
            catch (Exception ex)
            {
                Volatile.Write(ref _lastError, $"{DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture)} " +
                                               $"{job.Describe}: {SqlSanitizer.Mask(ex.Message)}");

                if (attempt == attempts)
                {
                    _logger.Error($"[DB] Не удалось записать {job.Describe} за " +
                                  $"{attempts.ToString(CultureInfo.InvariantCulture)} попыт(ок). " +
                                  "Задание уходит в спул и будет дослано позже", ex);
                    Spool(job);
                    return;
                }

                // Экспоненциальная пауза: база могла перезагружаться.
                var wait = delay * attempt;
                try
                {
                    await Task.Delay(wait, token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    Spool(job);
                    return;
                }
            }
        }
    }

    private async Task WriteAsync(WriteJob job, CancellationToken token)
    {
        await using var connection = _db.CreateConnection();
        await connection.OpenAsync(token).ConfigureAwait(false);

        switch (job)
        {
            case ServerUpsertJob server:
                await WriteServerAsync(connection, server, token).ConfigureAwait(false);
                break;
            case SessionOpenJob open:
                await WriteOpenAsync(connection, open, token).ConfigureAwait(false);
                break;
            case SessionCloseJob close:
                await WriteCloseAsync(connection, close, token).ConfigureAwait(false);
                break;
            case OnlineSnapshotJob snapshot:
                await WriteSnapshotAsync(connection, snapshot, token).ConfigureAwait(false);
                break;
            default:
                _logger.Error($"[DB] Неизвестный тип задания: {job.GetType().Name}");
                break;
        }
    }

    private MySqlCommand NewCommand(MySqlConnection connection, string sql)
    {
        var command = connection.CreateCommand();
        command.CommandTimeout = (int)_db.CommandTimeoutSeconds;
        command.CommandText = sql;
        return command;
    }

    /// Пустой адрес НЕ затирает уже записанный.
    ///
    /// Это корень проблемы «в базе 0.0.0.0, и правка руками не держится»: раньше
    /// адрес перезаписывался при КАЖДОМ старте, поэтому исправленная вручную строка
    /// возвращалась к мусору на следующем рестарте. Теперь отсутствие адреса —
    /// это «нечего сказать», а не «сотри то, что есть».
    ///
    /// Вынесено в отдельный метод, чтобы это свойство проверялось тестом:
    /// условие легко потерять при следующей правке запроса.
    internal static string ServerUpsertSql(string prefix) =>
        $"INSERT INTO `{prefix}servers` (`id`, `address`, `hostname`, `first_seen`, `last_seen`) " +
        "VALUES (@id, @address, @hostname, @now, @now) " +
        "ON DUPLICATE KEY UPDATE " +
        "`address` = IF(VALUES(`address`) = '', `address`, VALUES(`address`)), " +
        "`hostname` = IF(VALUES(`hostname`) = '', `hostname`, VALUES(`hostname`)), " +
        "`last_seen` = VALUES(`last_seen`)";

    private async Task WriteServerAsync(MySqlConnection connection, ServerUpsertJob job, CancellationToken token)
    {
        await using var command = NewCommand(connection, ServerUpsertSql(_db.Prefix));

        command.Parameters.AddWithValue("@id", job.ServerId);
        command.Parameters.AddWithValue("@address", job.Address);
        command.Parameters.AddWithValue("@hostname", job.Hostname);
        command.Parameters.AddWithValue("@now", job.SeenAt);
        await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
    }

    /// INSERT IGNORE, а не просто INSERT: задание может прийти повторно из спула,
    /// и дубль по session_key не должен превращаться в ошибку записи.
    private async Task WriteOpenAsync(MySqlConnection connection, SessionOpenJob job, CancellationToken token)
    {
        await using (var command = NewCommand(connection,
            $"INSERT IGNORE INTO `{_db.Prefix}sessions` " +
            "(`session_key`, `steamid64`, `account_id`, `server_id`, `nickname`, `started_at`, " +
            " `connect_map`, `players_online`, `max_players`, `client_lang`, `player_ip`, `ip_hash`, " +
            " `ip_subnet`, `country_iso`, `country_name`, `city`, `plugin_version`) " +
            "VALUES (@key, @steam, @account, @server, @nick, @started, @map, @online, @max, @lang, " +
            " @ip, @hash, @subnet, @iso, @country, @city, @version)"))
        {
            command.Parameters.AddWithValue("@key", job.SessionKey);
            command.Parameters.AddWithValue("@steam", job.SteamId64);
            command.Parameters.AddWithValue("@account", job.AccountId);
            command.Parameters.AddWithValue("@server", job.ServerId);
            command.Parameters.AddWithValue("@nick", job.Nickname);
            command.Parameters.AddWithValue("@started", job.StartedAt);
            command.Parameters.AddWithValue("@map", job.ConnectMap);
            command.Parameters.AddWithValue("@online", job.PlayersOnline);
            command.Parameters.AddWithValue("@max", job.MaxPlayers);
            command.Parameters.AddWithValue("@lang", (object?)job.ClientLang ?? DBNull.Value);
            command.Parameters.AddWithValue("@ip", (object?)job.PlayerIp ?? DBNull.Value);
            command.Parameters.AddWithValue("@hash", (object?)job.IpHash ?? DBNull.Value);
            command.Parameters.AddWithValue("@subnet", (object?)job.IpSubnet ?? DBNull.Value);
            command.Parameters.AddWithValue("@iso", (object?)job.CountryIso ?? DBNull.Value);
            command.Parameters.AddWithValue("@country", (object?)job.CountryName ?? DBNull.Value);
            command.Parameters.AddWithValue("@city", (object?)job.City ?? DBNull.Value);
            command.Parameters.AddWithValue("@version", job.PluginVersion);

            await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
        }

        // Игрок существует с первого захода, даже если сессия никогда не закроется.
        await using var upsert = NewCommand(connection,
            $"INSERT INTO `{_db.Prefix}players` " +
            "(`steamid64`, `account_id`, `first_seen`, `last_seen`, `last_nickname`, `last_country`, `last_server_id`) " +
            "VALUES (@steam, @account, @now, @now, @nick, @iso, @server) " +
            "ON DUPLICATE KEY UPDATE `last_seen` = VALUES(`last_seen`), " +
            "`last_nickname` = VALUES(`last_nickname`), " +
            "`last_country` = COALESCE(VALUES(`last_country`), `last_country`), " +
            "`last_server_id` = VALUES(`last_server_id`)");

        upsert.Parameters.AddWithValue("@steam", job.SteamId64);
        upsert.Parameters.AddWithValue("@account", job.AccountId);
        upsert.Parameters.AddWithValue("@now", job.StartedAt);
        upsert.Parameters.AddWithValue("@nick", job.Nickname);
        upsert.Parameters.AddWithValue("@iso", (object?)job.CountryIso ?? DBNull.Value);
        upsert.Parameters.AddWithValue("@server", job.ServerId);
        await upsert.ExecuteNonQueryAsync(token).ConfigureAwait(false);
    }

    /// Закрытие сессии идемпотентно.
    ///
    /// UPDATE ставит условие ended_at IS NULL. Если он ничего не изменил — сессия уже
    /// закрыта (задание пришло повторно из спула) или строки открытия вообще нет
    /// (её задание потерялось). Первый случай — выходим и НЕ трогаем счётчики, иначе
    /// повтор задания удваивал бы наигранное время. Второй — вставляем полную строку,
    /// чтобы данные о сессии не пропали совсем.
    private async Task WriteCloseAsync(MySqlConnection connection, SessionCloseJob job, CancellationToken token)
    {
        await using var transaction = await connection.BeginTransactionAsync(token).ConfigureAwait(false);

        var stats = job.Stats;
        int affected;

        await using (var command = NewCommand(connection,
            $"UPDATE `{_db.Prefix}sessions` SET " +
            // spectator_seconds обязан быть здесь, а не только в INSERT ниже: обычное
            // закрытие сессии идёт именно этим UPDATE, и без колонки время вне игры
            // оставалось NULL у каждой нормально закрытой сессии. Заметить это тестом,
            // который шлёт одно задание закрытия без открытия, невозможно — такой
            // сценарий уходит в ветку INSERT, где колонка есть.
            "`ended_at` = @ended, `duration_seconds` = @duration, `spectator_seconds` = @spectator, " +
            "`end_kind` = @kind, " +
            "`nickname` = @nick, `disconnect_map` = @map, `disconnect_reason` = @reason, " +
            "`disconnect_reason_name` = @reason_name, `kills` = @kills, `deaths` = @deaths, " +
            "`assists` = @assists, `headshots` = @headshots, `damage` = @damage, `mvp` = @mvp, " +
            "`score` = @score, `rounds_played` = @rounds, `team_final` = @team, " +
            "`team_changes` = @team_changes, `ping_avg` = @ping_avg, `ping_min` = @ping_min, " +
            "`ping_max` = @ping_max, `ping_samples` = @ping_samples " +
            "WHERE `session_key` = @key AND `ended_at` IS NULL"))
        {
            command.Transaction = transaction;
            AddCloseParameters(command, job, stats);
            affected = await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
        }

        if (affected == 0)
        {
            await using var exists = NewCommand(connection,
                $"SELECT 1 FROM `{_db.Prefix}sessions` WHERE `session_key` = @key");
            exists.Transaction = transaction;
            exists.Parameters.AddWithValue("@key", job.SessionKey);

            var found = await exists.ExecuteScalarAsync(token).ConfigureAwait(false);
            if (found != null)
            {
                // Сессия уже закрыта — повторное задание из спула. Счётчики не трогаем.
                await transaction.CommitAsync(token).ConfigureAwait(false);
                return;
            }

            await InsertClosedSessionAsync(connection, transaction, job, stats, token).ConfigureAwait(false);
        }

        await UpdateAggregatesAsync(connection, transaction, job, token).ConfigureAwait(false);
        await transaction.CommitAsync(token).ConfigureAwait(false);
    }

    private static void AddCloseParameters(MySqlCommand command, SessionCloseJob job, MatchStatsSnapshot? stats)
    {
        command.Parameters.AddWithValue("@key", job.SessionKey);
        command.Parameters.AddWithValue("@ended", job.EndedAt);
        command.Parameters.AddWithValue("@duration", job.DurationSeconds);
        command.Parameters.AddWithValue("@spectator", job.SpectatorSeconds);
        command.Parameters.AddWithValue("@kind", (byte)job.EndKind);
        command.Parameters.AddWithValue("@nick", job.Nickname);
        command.Parameters.AddWithValue("@map", job.DisconnectMap);
        command.Parameters.AddWithValue("@reason", job.DisconnectReason);
        command.Parameters.AddWithValue("@reason_name", job.DisconnectReasonName);
        command.Parameters.AddWithValue("@kills", (object?)stats?.Kills ?? DBNull.Value);
        command.Parameters.AddWithValue("@deaths", (object?)stats?.Deaths ?? DBNull.Value);
        command.Parameters.AddWithValue("@assists", (object?)stats?.Assists ?? DBNull.Value);
        command.Parameters.AddWithValue("@headshots", (object?)stats?.HeadShots ?? DBNull.Value);
        command.Parameters.AddWithValue("@damage", (object?)stats?.Damage ?? DBNull.Value);
        command.Parameters.AddWithValue("@mvp", (object?)stats?.Mvps ?? DBNull.Value);
        command.Parameters.AddWithValue("@score", (object?)stats?.Score ?? DBNull.Value);
        command.Parameters.AddWithValue("@rounds", (object?)stats?.RoundsPlayed ?? DBNull.Value);
        command.Parameters.AddWithValue("@team", (object?)stats?.Team ?? DBNull.Value);
        command.Parameters.AddWithValue("@team_changes", (object?)stats?.TeamChanges ?? DBNull.Value);
        command.Parameters.AddWithValue("@ping_avg", job.PingSamples > 0 ? job.PingAvg : DBNull.Value);
        command.Parameters.AddWithValue("@ping_min", job.PingSamples > 0 ? job.PingMin : DBNull.Value);
        command.Parameters.AddWithValue("@ping_max", job.PingSamples > 0 ? job.PingMax : DBNull.Value);
        command.Parameters.AddWithValue("@ping_samples", job.PingSamples);
    }

    private async Task InsertClosedSessionAsync(MySqlConnection connection, MySqlTransaction transaction,
        SessionCloseJob job, MatchStatsSnapshot? stats, CancellationToken token)
    {
        await using var command = NewCommand(connection,
            $"INSERT IGNORE INTO `{_db.Prefix}sessions` " +
            "(`session_key`, `steamid64`, `account_id`, `server_id`, `nickname`, `started_at`, " +
            " `ended_at`, `duration_seconds`, `spectator_seconds`, `end_kind`, `disconnect_map`, " +
            " `disconnect_reason`, " +
            " `disconnect_reason_name`, `kills`, `deaths`, `assists`, `headshots`, `damage`, `mvp`, " +
            " `score`, `rounds_played`, `team_final`, `team_changes`, `ping_avg`, `ping_min`, " +
            " `ping_max`, `ping_samples`) " +
            "VALUES (@key, @steam, @account, @server, @nick, @started, @ended, @duration, @spectator, @kind, " +
            " @map, @reason, @reason_name, @kills, @deaths, @assists, @headshots, @damage, @mvp, " +
            " @score, @rounds, @team, @team_changes, @ping_avg, @ping_min, @ping_max, @ping_samples)");

        command.Transaction = transaction;
        AddCloseParameters(command, job, stats);
        command.Parameters.AddWithValue("@steam", job.SteamId64);
        command.Parameters.AddWithValue("@account", job.AccountId);
        command.Parameters.AddWithValue("@server", job.ServerId);
        command.Parameters.AddWithValue("@started", job.StartedAt);

        await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
    }

    private async Task UpdateAggregatesAsync(MySqlConnection connection, MySqlTransaction transaction,
        SessionCloseJob job, CancellationToken token)
    {
        await using (var players = NewCommand(connection,
            $"INSERT INTO `{_db.Prefix}players` " +
            "(`steamid64`, `account_id`, `first_seen`, `last_seen`, `sessions_count`, `total_seconds`, " +
            " `last_nickname`, `last_server_id`) " +
            "VALUES (@steam, @account, @started, @ended, 1, @duration, @nick, @server) " +
            "ON DUPLICATE KEY UPDATE `last_seen` = VALUES(`last_seen`), " +
            "`sessions_count` = `sessions_count` + 1, " +
            "`total_seconds` = `total_seconds` + VALUES(`total_seconds`), " +
            "`last_nickname` = VALUES(`last_nickname`), `last_server_id` = VALUES(`last_server_id`)"))
        {
            players.Transaction = transaction;
            players.Parameters.AddWithValue("@steam", job.SteamId64);
            players.Parameters.AddWithValue("@account", job.AccountId);
            players.Parameters.AddWithValue("@started", job.StartedAt);
            players.Parameters.AddWithValue("@ended", job.EndedAt);
            // В «наиграно» уходит либо вся сессия, либо время в составе команды —
            // по настройке, снятой на момент закрытия сессии.
            players.Parameters.AddWithValue(
                "@duration",
                job.CountSpectatorTime
                    ? job.DurationSeconds
                    : Math.Max(0, job.DurationSeconds - job.SpectatorSeconds));
            players.Parameters.AddWithValue("@nick", job.Nickname);
            players.Parameters.AddWithValue("@server", job.ServerId);
            await players.ExecuteNonQueryAsync(token).ConfigureAwait(false);
        }

        if (string.IsNullOrEmpty(job.Nickname)) return;

        await using var nicknames = NewCommand(connection,
            $"INSERT INTO `{_db.Prefix}nicknames` (`steamid64`, `nickname`, `first_seen`, `last_seen`, `times_seen`) " +
            "VALUES (@steam, @nick, @started, @ended, 1) " +
            "ON DUPLICATE KEY UPDATE `last_seen` = VALUES(`last_seen`), `times_seen` = `times_seen` + 1");

        nicknames.Transaction = transaction;
        nicknames.Parameters.AddWithValue("@steam", job.SteamId64);
        nicknames.Parameters.AddWithValue("@nick", job.Nickname);
        nicknames.Parameters.AddWithValue("@started", job.StartedAt);
        nicknames.Parameters.AddWithValue("@ended", job.EndedAt);
        await nicknames.ExecuteNonQueryAsync(token).ConfigureAwait(false);
    }

    private async Task WriteSnapshotAsync(MySqlConnection connection, OnlineSnapshotJob job, CancellationToken token)
    {
        await using var command = NewCommand(connection,
            $"INSERT INTO `{_db.Prefix}online_snapshots` " +
            "(`server_id`, `taken_at`, `players`, `bots`, `max_players`, `map`) " +
            "VALUES (@server, @taken, @players, @bots, @max, @map)");

        command.Parameters.AddWithValue("@server", job.ServerId);
        command.Parameters.AddWithValue("@taken", job.TakenAt);
        command.Parameters.AddWithValue("@players", job.Players);
        command.Parameters.AddWithValue("@bots", job.Bots);
        command.Parameters.AddWithValue("@max", job.MaxPlayers);
        command.Parameters.AddWithValue("@map", job.Map);
        await command.ExecuteNonQueryAsync(token).ConfigureAwait(false);
    }

    // ---- Спул ------------------------------------------------------------------

    /// Запись, которую база не приняла, ложится на диск рядом с плагином.
    /// Это единственная защита от потери данных при недоступной БД: процесс игрового
    /// сервера умирает без предупреждения, и всё, что жило только в памяти, исчезает.
    private void Spool(WriteJob job)
    {
        if (!_storage.SpoolEnabled)
        {
            _logger.Error($"[DB] Спул выключен — задание потеряно: {job.Describe}");
            return;
        }

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_spoolPath)!);

            if (CountSpoolLines() >= _storage.SpoolMaxEntries)
            {
                _logger.Error($"[DB] Спул достиг предела ({_storage.SpoolMaxEntries.ToString(CultureInfo.InvariantCulture)} " +
                              "записей) — задание отброшено. Проверьте доступность базы");
                return;
            }

            var line = JsonSerializer.Serialize(job, typeof(WriteJob), SpoolOptions);
            File.AppendAllText(_spoolPath, line + Environment.NewLine, Encoding.UTF8);
            ProtectFile(_spoolPath);
            Interlocked.Increment(ref _spooled);
        }
        catch (Exception ex)
        {
            _logger.Error($"[DB] Не удалось записать задание в спул: {job.Describe}", ex);
        }
    }

    private int CountSpoolLines()
    {
        if (!File.Exists(_spoolPath)) return 0;

        var count = 0;
        foreach (var _ in File.ReadLines(_spoolPath)) count++;
        return count;
    }

    /// Спул содержит ники и IP игроков — файл не должен читаться кем попало на shared-хостинге.
    private static void ProtectFile(string path)
    {
        if (OperatingSystem.IsWindows()) return;

        try
        {
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // Права — не повод терять данные
        }
    }

    private async Task TryReplaySpoolAsync(CancellationToken token)
    {
        if (!File.Exists(_spoolPath)) return;
        if (DateTime.UtcNow - _lastSpoolReplay < TimeSpan.FromSeconds(60)) return;

        await ReplaySpoolAsync(token).ConfigureAwait(false);
    }

    /// Досылка спула. Файл читается целиком и удаляется ДО отправки: если запись снова
    /// не пройдёт, задания вернутся в спул обычным путём, а не задвоятся.
    private async Task ReplaySpoolAsync(CancellationToken token)
    {
        _lastSpoolReplay = DateTime.UtcNow;

        List<string> lines;
        try
        {
            if (!File.Exists(_spoolPath)) return;
            lines = [.. await File.ReadAllLinesAsync(_spoolPath, token).ConfigureAwait(false)];
            File.Delete(_spoolPath);
        }
        catch (Exception ex)
        {
            _logger.Error("[DB] Не удалось прочитать спул", ex);
            return;
        }

        if (lines.Count == 0) return;

        _logger.Info($"[DB] Досылаю {lines.Count.ToString(CultureInfo.InvariantCulture)} отложенных записей");

        foreach (var line in lines)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;

            WriteJob? job;
            try
            {
                job = JsonSerializer.Deserialize<WriteJob>(line, SpoolOptions);
            }
            catch (JsonException ex)
            {
                _logger.Error($"[DB] Битая строка в спуле пропущена: {ex.Message}");
                continue;
            }

            if (job == null) continue;

            job.Attempts = 0;
            await ProcessAsync(job, token).ConfigureAwait(false);
        }
    }

    // ---- Остановка -------------------------------------------------------------

    /// Плагин выгружают или сервер останавливают: даём писателю доработать очередь,
    /// а всё, что не успело, кладём в спул. Ждать бесконечно нельзя — выгрузка плагина
    /// не должна вешать сервер.
    public async ValueTask DisposeAsync()
    {
        _channel.Writer.TryComplete();

        try
        {
            await _loop.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            _logger.Warn("[DB] Писатель не успел за 5 секунд — остаток уходит в спул");
        }
        catch (Exception ex)
        {
            _logger.Error("[DB] Ошибка при остановке писателя", ex);
        }

        await _cts.CancelAsync().ConfigureAwait(false);

        while (_channel.Reader.TryRead(out var job))
            Spool(job);

        _cts.Dispose();
    }
}
