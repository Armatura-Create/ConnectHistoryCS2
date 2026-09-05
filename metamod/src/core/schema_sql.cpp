#include "core/schema_sql.h"

namespace ch {

std::vector<std::string> BuildSchema(const std::string& p) {
    std::vector<std::string> ddl;

    ddl.push_back(
        "CREATE TABLE IF NOT EXISTS `" + p + "schema_version` (\n"
        "  `k` VARCHAR(32) NOT NULL,\n"
        "  `v` INT NOT NULL,\n"
        "  `updated_at` DATETIME NOT NULL,\n"
        "  PRIMARY KEY (`k`)\n"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    ddl.push_back(
        "CREATE TABLE IF NOT EXISTS `" + p + "servers` (\n"
        "  `id` INT NOT NULL,\n"
        "  `address` VARCHAR(64) NOT NULL DEFAULT '',\n"
        "  `hostname` VARCHAR(128) NOT NULL DEFAULT '',\n"
        "  `first_seen` DATETIME NOT NULL,\n"
        "  `last_seen` DATETIME NOT NULL,\n"
        "  PRIMARY KEY (`id`)\n"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    ddl.push_back(
        "CREATE TABLE IF NOT EXISTS `" + p + "players` (\n"
        "  `steamid64` BIGINT UNSIGNED NOT NULL,\n"
        "  `account_id` INT UNSIGNED NOT NULL,\n"
        "  `first_seen` DATETIME NOT NULL,\n"
        "  `last_seen` DATETIME NOT NULL,\n"
        "  `sessions_count` INT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `total_seconds` BIGINT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `last_nickname` VARCHAR(128) NOT NULL DEFAULT '',\n"
        "  `last_country` CHAR(2) NULL DEFAULT NULL,\n"
        "  `last_server_id` INT NULL DEFAULT NULL,\n"
        "  PRIMARY KEY (`steamid64`),\n"
        "  KEY `idx_last_seen` (`last_seen`),\n"
        "  KEY `idx_total_seconds` (`total_seconds`)\n"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    ddl.push_back(
        "CREATE TABLE IF NOT EXISTS `" + p + "nicknames` (\n"
        "  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,\n"
        "  `steamid64` BIGINT UNSIGNED NOT NULL,\n"
        "  `nickname` VARCHAR(128) NOT NULL,\n"
        "  `first_seen` DATETIME NOT NULL,\n"
        "  `last_seen` DATETIME NOT NULL,\n"
        "  `times_seen` INT UNSIGNED NOT NULL DEFAULT 1,\n"
        "  PRIMARY KEY (`id`),\n"
        "  UNIQUE KEY `uq_player_nickname` (`steamid64`, `nickname`),\n"
        "  KEY `idx_nickname` (`nickname`)\n"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    // Ядро. Строка создаётся на ВХОДЕ игрока (ended_at IS NULL) и закрывается
    // на выходе. Поэтому «кто сейчас онлайн» — это ended_at IS NULL AND end_kind = 0,
    // а оборванная сессия (сервер умер) остаётся видимым фактом, а не исчезает.
    ddl.push_back(
        "CREATE TABLE IF NOT EXISTS `" + p + "sessions` (\n"
        "  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,\n"
        "  `session_key` CHAR(32) NOT NULL,\n"
        "  `steamid64` BIGINT UNSIGNED NOT NULL,\n"
        "  `account_id` INT UNSIGNED NOT NULL,\n"
        "  `server_id` INT NOT NULL,\n"
        "  `nickname` VARCHAR(128) NOT NULL DEFAULT '',\n"
        "  `started_at` DATETIME NOT NULL,\n"
        "  `ended_at` DATETIME NULL DEFAULT NULL,\n"
        "  `duration_seconds` INT UNSIGNED NULL DEFAULT NULL,\n"
        "  `spectator_seconds` INT UNSIGNED NULL DEFAULT NULL,\n"
        "  `end_kind` TINYINT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `connect_map` VARCHAR(64) NOT NULL DEFAULT '',\n"
        "  `disconnect_map` VARCHAR(64) NULL DEFAULT NULL,\n"
        "  `disconnect_reason` SMALLINT NULL DEFAULT NULL,\n"
        "  `disconnect_reason_name` VARCHAR(64) NULL DEFAULT NULL,\n"
        "  `players_online` SMALLINT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `max_players` SMALLINT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `client_lang` VARCHAR(8) NULL DEFAULT NULL,\n"
        "  `player_ip` VARCHAR(45) NULL DEFAULT NULL,\n"
        "  `ip_hash` CHAR(64) NULL DEFAULT NULL,\n"
        "  `ip_subnet` VARCHAR(45) NULL DEFAULT NULL,\n"
        "  `country_iso` CHAR(2) NULL DEFAULT NULL,\n"
        "  `country_name` VARCHAR(64) NULL DEFAULT NULL,\n"
        "  `city` VARCHAR(128) NULL DEFAULT NULL,\n"
        "  `kills` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `deaths` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `assists` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `headshots` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `damage` INT UNSIGNED NULL DEFAULT NULL,\n"
        "  `mvp` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `score` INT NULL DEFAULT NULL,\n"
        "  `rounds_played` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `team_final` TINYINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `team_changes` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `ping_avg` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `ping_min` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `ping_max` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `ping_samples` SMALLINT UNSIGNED NULL DEFAULT NULL,\n"
        "  `plugin_version` VARCHAR(32) NOT NULL DEFAULT '',\n"
        "  PRIMARY KEY (`id`),\n"
        "  UNIQUE KEY `uq_session_key` (`session_key`),\n"
        "  KEY `idx_player_time` (`steamid64`, `started_at`),\n"
        "  KEY `idx_server_time` (`server_id`, `started_at`),\n"
        "  KEY `idx_started_at` (`started_at`),\n"
        "  KEY `idx_open` (`ended_at`)\n"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    ddl.push_back(
        "CREATE TABLE IF NOT EXISTS `" + p + "online_snapshots` (\n"
        "  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,\n"
        "  `server_id` INT NOT NULL,\n"
        "  `taken_at` DATETIME NOT NULL,\n"
        "  `players` SMALLINT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `bots` SMALLINT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `max_players` SMALLINT UNSIGNED NOT NULL DEFAULT 0,\n"
        "  `map` VARCHAR(64) NOT NULL DEFAULT '',\n"
        "  PRIMARY KEY (`id`),\n"
        "  KEY `idx_server_time` (`server_id`, `taken_at`)\n"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    return ddl;
}

std::vector<MigrationStep> Migrations(const std::string& prefix) {
    std::vector<MigrationStep> steps;

    // v2: убрать из servers.address значения, которые адресом не являются.
    //
    // Версия 1 писала туда результат ConVar ip, а тот при обычной настройке равен
    // 0.0.0.0 — это адрес привязки сокета, а не адрес сервера. Пустая строка честнее:
    // она означает «адрес неизвестен» и не перезаписывается пустым значением при
    // следующем старте. UPDATE, а не DELETE: строка сервера нужна, испорчено одно поле.
    steps.push_back({2,
                     "UPDATE `" + prefix + "servers` SET `address` = '' "
                     "WHERE `address` LIKE '0.0.0.0%' "
                     "   OR `address` LIKE '[::]%' "
                     "   OR `address` LIKE ':%'"});

    // v3: время, проведённое наблюдателем и без команды.
    //
    // Пишется всегда, независимо от Collect.CountSpectatorTime: настройка решает
    // лишь, вычитать ли его из «наиграно». Данные важнее настройки — передумав,
    // владелец сервера пересчитает агрегат, а не потеряет историю.
    //
    // Просто ALTER, без условий: у ALTER в MySQL нет формы IF NOT EXISTS (это
    // MariaDB), а на свежей базе колонка уже приезжает из BuildSchema. Ошибку
    // «колонка уже существует» вызывающая сторона обязана трактовать как
    // «шаг применён» — см. IsAlreadyAppliedError.
    steps.push_back({3,
                     "ALTER TABLE `" + prefix + "sessions` ADD COLUMN `spectator_seconds` "
                     "INT UNSIGNED NULL DEFAULT NULL AFTER `duration_seconds`"});

    return steps;
}

bool IsAlreadyAppliedError(unsigned int mysqlErrno) {
    return mysqlErrno == 1050 || mysqlErrno == 1060 || mysqlErrno == 1061;
}

}  // namespace ch
