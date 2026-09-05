using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace ConnectHistory;

/// Чтение из базы для игроцких команд.
///
/// Отдельно от писателя сознательно: писатель обязан оставаться одной последовательной
/// очередью, а команды игроков — это редкие независимые запросы, которые не должны
/// вставать в неё в очередь за сессиями.
public sealed class QueryService
{
    private readonly DatabaseService _db;
    private readonly ILogger _logger;

    public QueryService(DatabaseService db, ILogger logger)
    {
        _db = db;
        _logger = logger;
    }

    public sealed record PlayerTotals(long TotalSeconds, int Sessions, DateTime FirstSeen);

    public sealed record RecentSession(DateTime StartedAt, int DurationSeconds, string Map);

    public async Task<PlayerTotals?> GetTotalsAsync(ulong steamId, CancellationToken token)
    {
        try
        {
            await using var connection = _db.CreateConnection();
            await connection.OpenAsync(token).ConfigureAwait(false);

            await using var command = connection.CreateCommand();
            command.CommandTimeout = (int)_db.CommandTimeoutSeconds;
            command.CommandText =
                $"SELECT `total_seconds`, `sessions_count`, `first_seen` FROM `{_db.Prefix}players` " +
                "WHERE `steamid64` = @steam";
            command.Parameters.AddWithValue("@steam", steamId);

            await using var reader = await command.ExecuteReaderAsync(token).ConfigureAwait(false);
            if (!await reader.ReadAsync(token).ConfigureAwait(false)) return null;

            return new PlayerTotals(
                reader.GetInt64(0),
                reader.GetInt32(1),
                reader.GetDateTime(2));
        }
        catch (Exception ex)
        {
            _logger.Error($"[DB] Запрос наигранного времени для {steamId} не удался", ex);
            return null;
        }
    }

    public async Task<List<RecentSession>?> GetRecentAsync(ulong steamId, int limit, CancellationToken token)
    {
        try
        {
            await using var connection = _db.CreateConnection();
            await connection.OpenAsync(token).ConfigureAwait(false);

            await using var command = connection.CreateCommand();
            command.CommandTimeout = (int)_db.CommandTimeoutSeconds;
            // LIMIT параметризован: значение приходит из конфига, а не из чата, но
            // склеивать числа в SQL — привычка, которая однажды прострелит ногу.
            command.CommandText =
                $"SELECT `started_at`, `duration_seconds`, `connect_map` FROM `{_db.Prefix}sessions` " +
                "WHERE `steamid64` = @steam AND `ended_at` IS NOT NULL " +
                "ORDER BY `started_at` DESC LIMIT @limit";
            command.Parameters.AddWithValue("@steam", steamId);
            command.Parameters.AddWithValue("@limit", Math.Clamp(limit, 1, 20));

            var result = new List<RecentSession>();
            await using var reader = await command.ExecuteReaderAsync(token).ConfigureAwait(false);

            while (await reader.ReadAsync(token).ConfigureAwait(false))
            {
                result.Add(new RecentSession(
                    reader.GetDateTime(0),
                    reader.IsDBNull(1) ? 0 : reader.GetInt32(1),
                    reader.IsDBNull(2) ? "" : reader.GetString(2)));
            }

            return result;
        }
        catch (Exception ex)
        {
            _logger.Error($"[DB] Запрос последних сессий для {steamId} не удался", ex);
            return null;
        }
    }
}
