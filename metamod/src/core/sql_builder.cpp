#include "core/sql_builder.h"

#include "core/util/timeutil.h"

#include <algorithm>

namespace ch {
namespace {

SqlValue Stat(const MatchStats& stats, int32_t value) {
    return stats.valid ? SqlValue::Int(value) : SqlValue::Null();
}

// Пинг пишем только если замеры были: ноль в колонке неотличим от «связь идеальная»
SqlValue Ping(const WriteJob& job, int32_t value) {
    return job.pingSamples > 0 ? SqlValue::Int(value) : SqlValue::Null();
}

// Параметры закрытия в порядке, общем для UPDATE и для INSERT.
void AppendCloseValues(const WriteJob& job, std::vector<SqlValue>* params) {
    params->push_back(SqlValue::Text(FormatSqlDateTime(job.endedAt)));
    params->push_back(SqlValue::Int(job.durationSeconds));
    params->push_back(SqlValue::Int(job.spectatorSeconds));
    params->push_back(SqlValue::Int(static_cast<int64_t>(job.endKind)));
    params->push_back(SqlValue::Text(job.nickname));
    params->push_back(SqlValue::Text(job.disconnectMap));
    params->push_back(SqlValue::Int(job.disconnectReason));
    params->push_back(SqlValue::Text(job.disconnectReasonName));
    params->push_back(Stat(job.stats, job.stats.kills));
    params->push_back(Stat(job.stats, job.stats.deaths));
    params->push_back(Stat(job.stats, job.stats.assists));
    params->push_back(Stat(job.stats, job.stats.headShots));
    params->push_back(Stat(job.stats, job.stats.damage));
    params->push_back(Stat(job.stats, job.stats.mvps));
    params->push_back(SqlValue::OptionalInt(job.stats.valid && job.stats.hasScore,
                                            job.stats.score));
    params->push_back(Stat(job.stats, job.stats.roundsPlayed));
    params->push_back(Stat(job.stats, job.stats.team));
    params->push_back(Stat(job.stats, job.stats.teamChanges));
    params->push_back(Ping(job, job.pingAvg));
    params->push_back(Ping(job, job.pingMin));
    params->push_back(Ping(job, job.pingMax));
    params->push_back(SqlValue::Int(job.pingSamples));
}

}  // namespace

SqlValue SqlValue::Int(int64_t value) {
    SqlValue v;
    v.kind = Kind::Int64;
    v.integer = value;
    return v;
}

SqlValue SqlValue::UInt(uint64_t value) {
    SqlValue v;
    v.kind = Kind::UInt64;
    v.unsignedInteger = value;
    return v;
}

SqlValue SqlValue::Text(const std::string& value) {
    SqlValue v;
    v.kind = Kind::Text;
    v.text = value;
    return v;
}

SqlValue SqlValue::OptionalText(bool present, const std::string& value) {
    return present ? Text(value) : Null();
}

SqlValue SqlValue::OptionalInt(bool present, int64_t value) {
    return present ? Int(value) : Null();
}

std::string ServerUpsertSql(const std::string& prefix) {
    return "INSERT INTO `" + prefix +
           "servers` (`id`, `address`, `hostname`, `first_seen`, `last_seen`) "
           "VALUES (?, ?, ?, ?, ?) "
           "ON DUPLICATE KEY UPDATE "
           "`address` = IF(VALUES(`address`) = '', `address`, VALUES(`address`)), "
           "`hostname` = IF(VALUES(`hostname`) = '', `hostname`, VALUES(`hostname`)), "
           "`last_seen` = VALUES(`last_seen`)";
}

Statement BuildServerUpsert(const std::string& prefix, const WriteJob& job) {
    const std::string now = FormatSqlDateTime(job.seenAt);

    Statement statement;
    statement.sql = ServerUpsertSql(prefix);
    statement.params = {
        SqlValue::Int(job.serverId),
        SqlValue::Text(job.address),
        SqlValue::Text(job.hostname),
        SqlValue::Text(now),
        SqlValue::Text(now),
    };
    return statement;
}

std::vector<Statement> BuildSessionOpen(const std::string& prefix, const WriteJob& job) {
    const std::string started = FormatSqlDateTime(job.startedAt);

    Statement insert;
    insert.sql =
        "INSERT IGNORE INTO `" + prefix + "sessions` "
        "(`session_key`, `steamid64`, `account_id`, `server_id`, `nickname`, `started_at`, "
        " `connect_map`, `players_online`, `max_players`, `client_lang`, `player_ip`, `ip_hash`, "
        " `ip_subnet`, `country_iso`, `country_name`, `city`, `plugin_version`) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    insert.params = {
        SqlValue::Text(job.sessionKey),
        SqlValue::UInt(job.steamId64),
        SqlValue::UInt(job.accountId),
        SqlValue::Int(job.serverId),
        SqlValue::Text(job.nickname),
        SqlValue::Text(started),
        SqlValue::Text(job.connectMap),
        SqlValue::Int(job.playersOnline),
        SqlValue::Int(job.maxPlayers),
        SqlValue::OptionalText(job.hasClientLang, job.clientLang),
        SqlValue::OptionalText(job.hasPlayerIp, job.playerIp),
        SqlValue::OptionalText(job.hasIpHash, job.ipHash),
        SqlValue::OptionalText(job.hasIpSubnet, job.ipSubnet),
        SqlValue::OptionalText(job.hasCountryIso, job.countryIso),
        SqlValue::OptionalText(job.hasCountryName, job.countryName),
        SqlValue::OptionalText(job.hasCity, job.city),
        SqlValue::Text(job.pluginVersion),
    };

    // Игрок существует с первого захода, даже если сессия никогда не закроется.
    Statement upsert;
    upsert.sql =
        "INSERT INTO `" + prefix + "players` "
        "(`steamid64`, `account_id`, `first_seen`, `last_seen`, `last_nickname`, "
        " `last_country`, `last_server_id`) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE `last_seen` = VALUES(`last_seen`), "
        "`last_nickname` = VALUES(`last_nickname`), "
        "`last_country` = COALESCE(VALUES(`last_country`), `last_country`), "
        "`last_server_id` = VALUES(`last_server_id`)";
    upsert.params = {
        SqlValue::UInt(job.steamId64),
        SqlValue::UInt(job.accountId),
        SqlValue::Text(started),
        SqlValue::Text(started),
        SqlValue::Text(job.nickname),
        SqlValue::OptionalText(job.hasCountryIso, job.countryIso),
        SqlValue::Int(job.serverId),
    };

    return {insert, upsert};
}

Statement BuildSessionCloseUpdate(const std::string& prefix, const WriteJob& job) {
    Statement statement;
    // spectator_seconds обязан быть здесь, а не только в INSERT: обычное закрытие
    // сессии идёт именно этим UPDATE. В C#-версии колонки тут не было, и время вне
    // игры оставалось NULL у каждой нормально закрытой сессии — исправлено там же.
    statement.sql =
        "UPDATE `" + prefix + "sessions` SET "
        "`ended_at` = ?, `duration_seconds` = ?, `spectator_seconds` = ?, `end_kind` = ?, "
        "`nickname` = ?, `disconnect_map` = ?, `disconnect_reason` = ?, "
        "`disconnect_reason_name` = ?, `kills` = ?, `deaths` = ?, "
        "`assists` = ?, `headshots` = ?, `damage` = ?, `mvp` = ?, "
        "`score` = ?, `rounds_played` = ?, `team_final` = ?, "
        "`team_changes` = ?, `ping_avg` = ?, `ping_min` = ?, "
        "`ping_max` = ?, `ping_samples` = ? "
        "WHERE `session_key` = ? AND `ended_at` IS NULL";

    AppendCloseValues(job, &statement.params);
    statement.params.push_back(SqlValue::Text(job.sessionKey));
    return statement;
}

Statement BuildSessionExists(const std::string& prefix, const WriteJob& job) {
    Statement statement;
    statement.sql = "SELECT 1 FROM `" + prefix + "sessions` WHERE `session_key` = ?";
    statement.params = {SqlValue::Text(job.sessionKey)};
    return statement;
}

Statement BuildSessionCloseInsert(const std::string& prefix, const WriteJob& job) {
    Statement statement;
    statement.sql =
        "INSERT IGNORE INTO `" + prefix + "sessions` "
        "(`ended_at`, `duration_seconds`, `spectator_seconds`, `end_kind`, `nickname`, "
        " `disconnect_map`, `disconnect_reason`, `disconnect_reason_name`, `kills`, `deaths`, "
        " `assists`, `headshots`, `damage`, `mvp`, `score`, `rounds_played`, `team_final`, "
        " `team_changes`, `ping_avg`, `ping_min`, `ping_max`, `ping_samples`, "
        " `session_key`, `steamid64`, `account_id`, `server_id`, `started_at`) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    AppendCloseValues(job, &statement.params);
    statement.params.push_back(SqlValue::Text(job.sessionKey));
    statement.params.push_back(SqlValue::UInt(job.steamId64));
    statement.params.push_back(SqlValue::UInt(job.accountId));
    statement.params.push_back(SqlValue::Int(job.serverId));
    statement.params.push_back(SqlValue::Text(FormatSqlDateTime(job.startedAt)));
    return statement;
}

std::vector<Statement> BuildAggregates(const std::string& prefix, const WriteJob& job) {
    std::vector<Statement> statements;

    // В «наиграно» уходит либо вся сессия, либо время в составе команды —
    // по настройке, снятой на момент ЗАКРЫТИЯ сессии: задание могло пролежать
    // в спуле, а строка обязана попасть в базу по тем правилам, по которым собрана.
    const int64_t counted =
        job.countSpectatorTime
            ? job.durationSeconds
            : std::max<int64_t>(0, static_cast<int64_t>(job.durationSeconds) -
                                       static_cast<int64_t>(job.spectatorSeconds));

    Statement players;
    players.sql =
        "INSERT INTO `" + prefix + "players` "
        "(`steamid64`, `account_id`, `first_seen`, `last_seen`, `sessions_count`, "
        " `total_seconds`, `last_nickname`, `last_server_id`) "
        "VALUES (?, ?, ?, ?, 1, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE `last_seen` = VALUES(`last_seen`), "
        "`sessions_count` = `sessions_count` + 1, "
        "`total_seconds` = `total_seconds` + VALUES(`total_seconds`), "
        "`last_nickname` = VALUES(`last_nickname`), `last_server_id` = VALUES(`last_server_id`)";
    players.params = {
        SqlValue::UInt(job.steamId64),
        SqlValue::UInt(job.accountId),
        SqlValue::Text(FormatSqlDateTime(job.startedAt)),
        SqlValue::Text(FormatSqlDateTime(job.endedAt)),
        SqlValue::Int(counted),
        SqlValue::Text(job.nickname),
        SqlValue::Int(job.serverId),
    };
    statements.push_back(players);

    if (job.nickname.empty()) return statements;

    Statement nicknames;
    nicknames.sql =
        "INSERT INTO `" + prefix + "nicknames` "
        "(`steamid64`, `nickname`, `first_seen`, `last_seen`, `times_seen`) "
        "VALUES (?, ?, ?, ?, 1) "
        "ON DUPLICATE KEY UPDATE `last_seen` = VALUES(`last_seen`), "
        "`times_seen` = `times_seen` + 1";
    nicknames.params = {
        SqlValue::UInt(job.steamId64),
        SqlValue::Text(job.nickname),
        SqlValue::Text(FormatSqlDateTime(job.startedAt)),
        SqlValue::Text(FormatSqlDateTime(job.endedAt)),
    };
    statements.push_back(nicknames);

    return statements;
}

Statement BuildOnlineSnapshot(const std::string& prefix, const WriteJob& job) {
    Statement statement;
    statement.sql =
        "INSERT INTO `" + prefix + "online_snapshots` "
        "(`server_id`, `taken_at`, `players`, `bots`, `max_players`, `map`) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    statement.params = {
        SqlValue::Int(job.serverId),
        SqlValue::Text(FormatSqlDateTime(job.takenAt)),
        SqlValue::Int(job.players),
        SqlValue::Int(job.bots),
        SqlValue::Int(job.maxPlayers),
        SqlValue::Text(job.map),
    };
    return statement;
}

CloseAction PlanClose(int affectedRows, bool rowExists) {
    if (affectedRows > 0) return CloseAction::UpdateAggregates;
    if (rowExists) return CloseAction::AlreadyClosed;
    return CloseAction::InsertThenAggregates;
}

}  // namespace ch
