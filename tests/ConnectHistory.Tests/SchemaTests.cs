using System;
using System.Linq;
using Xunit;

namespace ConnectHistory.Tests;

/// Схема создаётся на живом сервере при каждой загрузке плагина: DDL обязан быть
/// идемпотентным, а имена таблиц — уважать префикс из конфига.
public class SchemaTests
{
    [Fact]
    public void EveryStatementIsIdempotent()
    {
        foreach (var statement in SchemaService.BuildSchema("ch_"))
            Assert.Contains("CREATE TABLE IF NOT EXISTS", statement, StringComparison.Ordinal);
    }

    [Fact]
    public void AllTablesUseTheConfiguredPrefix()
    {
        var sql = string.Join('\n', SchemaService.BuildSchema("stats_"));

        foreach (var table in new[] { "schema_version", "servers", "players", "nicknames", "sessions", "online_snapshots" })
            Assert.Contains($"`stats_{table}`", sql, StringComparison.Ordinal);

        Assert.DoesNotContain("`ch_", sql, StringComparison.Ordinal);
    }

    [Fact]
    public void SessionsTableCarriesTheIndexesAnalyticsNeeds()
    {
        var sessions = SchemaService.BuildSchema("ch_").Single(s => s.Contains("`ch_sessions`", StringComparison.Ordinal));

        // Без них «последние заходы игрока» и «активность сервера» сканируют всю таблицу
        Assert.Contains("KEY `idx_player_time` (`steamid64`, `started_at`)", sessions, StringComparison.Ordinal);
        Assert.Contains("KEY `idx_server_time` (`server_id`, `started_at`)", sessions, StringComparison.Ordinal);
        Assert.Contains("KEY `idx_open` (`ended_at`)", sessions, StringComparison.Ordinal);

        // Ключ, по которому идёт UPDATE на закрытии сессии, обязан быть уникальным
        Assert.Contains("UNIQUE KEY `uq_session_key` (`session_key`)", sessions, StringComparison.Ordinal);
    }

    [Fact]
    public void AccountIdIsUnsigned()
    {
        // В signed int аккаунты за 2^31 записываются отрицательными числами
        var sessions = SchemaService.BuildSchema("ch_").Single(s => s.Contains("`ch_sessions`", StringComparison.Ordinal));
        Assert.Contains("`account_id` INT UNSIGNED NOT NULL", sessions, StringComparison.Ordinal);
        Assert.Contains("`steamid64` BIGINT UNSIGNED NOT NULL", sessions, StringComparison.Ordinal);
    }

    [Fact]
    public void SessionsAreOpenedBeforeTheyAreClosed()
    {
        // ended_at обязан допускать NULL: строка создаётся на входе игрока,
        // и оборванная сессия остаётся видимым фактом
        var sessions = SchemaService.BuildSchema("ch_").Single(s => s.Contains("`ch_sessions`", StringComparison.Ordinal));
        Assert.Contains("`ended_at` DATETIME NULL", sessions, StringComparison.Ordinal);
    }

    [Fact]
    public void SchemaVersionMatchesTheMigrationLadder()
    {
        // Версию поднимают только вместе с шагом миграции — иначе старые базы
        // не получат новых колонок
        var highest = SchemaService.Migrations("ch_").Keys.DefaultIfEmpty(1).Max();
        Assert.True(SchemaService.CurrentVersion >= highest,
            $"CurrentVersion {SchemaService.CurrentVersion} меньше последней миграции {highest}");
    }
}
