# ConnectHistory — схема базы и готовые запросы

Всё, что нужно, чтобы читать данные плагина из панели, отчёта или скрипта.
Таблицы создаются самим плагином при первом запуске, префикс задаётся
`Database.TablePrefix` (по умолчанию `ch_`).

> **Этот документ — контракт.** Плагин существует в трёх независимых реализациях
> (CounterStrikeSharp, SwiftlyS2, нативный Metamod:Source), и код у них не общий.
> Общая у них ровно эта схема: все три пишут одни и те же таблицы, и запросы ниже
> работают одинаково независимо от того, каким плагином записана строка.
> Немногочисленные расхождения перечислены в разделе
> [«Расхождения между целями»](#расхождения-между-целями) — читателю базы важно
> знать только их.

## Время

**Все метки времени в базе — UTC.** MySQL хранит `DATETIME` без часового пояса, поэтому
это соглашение держится строкой подключения: плагин ставит `DateTimeKind=Utc`, и драйвер
откажется записать значение с локальным `Kind`, а прочитанное вернёт помеченным как UTC.
Проверяется интеграционным тестом: колонка содержит ровно те цифры, что были отправлены.

Настройка `DisplayTimeZone` в `Settings.json` влияет **только** на то, в каком поясе время
показывают игрокам командами `playtime` и `lastseen`. На данные она не влияет
никак — читателям базы её можно игнорировать.

Сравнивать с текущим моментом нужно через `UTC_TIMESTAMP()`, а не `NOW()`: второй отдаёт
время в поясе сессии MySQL и на сервере с локальной таймзоной даст сдвиг.

## Главная идея схемы

**Строка сессии создаётся на ВХОДЕ игрока, а не на выходе.**

Из этого следует три вещи, на которых строятся почти все запросы ниже:

| Состояние строки | Что это значит |
|---|---|
| `ended_at IS NULL` и `end_kind = 0` | игрок **сейчас на сервере** |
| `ended_at IS NOT NULL` | сессия завершилась штатно, итоги заполнены |
| `ended_at IS NULL` и `end_kind = 5` | сервер завершился аварийно во время сессии |

Онлайн читается обычным `SELECT`, без RCON и A2S. Падения сервера видны в тех же
данных, отдельного мониторинга для этого не нужно.

## Таблицы

### `ch_sessions` — ядро

| Колонка | Тип | Смысл |
|---|---|---|
| `id` | BIGINT UNSIGNED | автоинкремент |
| `session_key` | CHAR(32) | ключ, сгенерированный плагином; по нему идёт закрытие сессии |
| `steamid64` | BIGINT UNSIGNED | полный SteamID64 |
| `account_id` | INT UNSIGNED | младшие 32 бита SteamID64 (привычный Steam account ID) |
| `server_id` | INT | `ServerId` из конфига сервера → `ch_servers.id` |
| `nickname` | VARCHAR(128) | ник на момент выхода (на входе, если сессия не закрыта) |
| `started_at` | DATETIME | вход, UTC |
| `ended_at` | DATETIME NULL | выход; `NULL` = сессия открыта |
| `duration_seconds` | INT UNSIGNED NULL | длительность подключения |
| `spectator_seconds` | INT UNSIGNED NULL | из них в наблюдателях и без команды; `NULL` — версия схемы < 3 |
| `end_kind` | TINYINT | 0 open, 1 disconnect, 2 map change, 3 shutdown, 4 plugin unload, 5 stale (аварийное завершение) |
| `connect_map` / `disconnect_map` | VARCHAR(64) | карта на входе и на выходе |
| `disconnect_reason` | SMALLINT NULL | код причины из перечисления Valve |
| `disconnect_reason_name` | VARCHAR(64) NULL | он же словами: `DISCONNECT_BY_USER`, `KICKED`, `STEAM_DROPPED`, … |
| `players_online` | SMALLINT UNSIGNED | сколько людей было на сервере в момент входа |
| `max_players` | SMALLINT UNSIGNED | слотов на сервере |
| `client_lang` | VARCHAR(8) NULL | язык клиента (`ru`, `en`) |
| `player_ip` | VARCHAR(45) NULL | IP игрока (если включён `Collect.PlayerIp`) |
| `ip_hash` | CHAR(64) NULL | HMAC-SHA256 от IP с солью из конфига |
| `ip_subnet` | VARCHAR(45) NULL | `/24` для IPv4, `/48` для IPv6 |
| `country_iso` / `country_name` / `city` | | GeoLite2 |
| `kills`, `deaths`, `assists`, `headshots`, `damage`, `mvp`, `score` | | итоги матча |
| `rounds_played` | SMALLINT UNSIGNED NULL | раундов, законченных при игроке |
| `team_final` | TINYINT NULL | 1 spectator, 2 T, 3 CT |
| `team_changes` | SMALLINT UNSIGNED NULL | сколько раз менял команду |
| `ping_avg`, `ping_min`, `ping_max`, `ping_samples` | | пинг за сессию, замеры раз в `PingSampleIntervalSeconds` |
| `plugin_version` | VARCHAR(32) | какая сборка плагина писала строку |

Индексы: `uq_session_key`, `(steamid64, started_at)`, `(server_id, started_at)`,
`(started_at)`, `(ended_at)`.

### `ch_players` — агрегаты

`steamid64` (PK), `account_id`, `first_seen`, `last_seen`, `sessions_count`,
`total_seconds`, `last_nickname`, `last_country`, `last_server_id`.

Обновляется при закрытии сессии в одной транзакции с ней, поэтому не расходится
с `ch_sessions`. Строка появляется уже на входе игрока — с нулевыми счётчиками.

### `ch_nicknames` — история ников

`steamid64` + `nickname` (уникальная пара), `first_seen`, `last_seen`, `times_seen`.

### `ch_servers` — справочник

`id` (PK, `ServerId` из конфига), `address` (`ip:port`), `hostname`, `first_seen`, `last_seen`.

Адрес сервера лежит здесь **один раз**, а не дублируется в каждой строке истории.

**Про `address`.** Процесс игрового сервера своего публичного адреса не знает:
ConVar `ip` отдаёт адрес *привязки сокета*, и при обычной настройке это `0.0.0.0`
(«слушаю все интерфейсы»). Поэтому адрес берётся из `Server.PublicAddress`
в `Settings.json`, а автоопределение — только запасной путь, результат которого
отбраковывается, если получился адрес привязки, loopback или приватная сеть.

Из этого следуют два свойства, на которые можно опираться:

- **Пустая строка означает «адрес неизвестен»**, а не «данных нет». Значения
  `0.0.0.0:27015` в колонке больше не появляются.
- **Пустое значение не затирает записанное.** Плагин обновляет `address` только
  когда ему есть что записать, поэтому один неудачный старт не портит справочник.

Базы, созданные версией схемы 1, чистятся автоматически при переходе на версию 2:
значения вида `0.0.0.0…` заменяются пустой строкой, строки серверов сохраняются.

### `ch_online_snapshots` — график посещаемости

`server_id`, `taken_at`, `players`, `bots`, `max_players`, `map`.
Точка снимается раз в `OnlineSnapshotIntervalSeconds` (по умолчанию 5 минут).

### `ch_schema_version`

`k` = `'schema'`, `v` — версия схемы. Плагин сам мигрирует базу вверх; читателям
эта таблица нужна только чтобы понимать, каких колонок ждать.

| Версия | Что появилось |
|---|---|
| 1 | Исходная схема |
| 2 | Чистка `ch_servers.address` от адресов привязки (`0.0.0.0…`) |
| 3 | Колонка `ch_sessions.spectator_seconds` |

### Наигранное время и наблюдатели

`duration_seconds` — это всегда время подключения. Сколько из него игрок провёл
наблюдателем или без команды, лежит в `spectator_seconds` и пишется **всегда**,
независимо от настроек.

На `ch_players.total_seconds` влияет `Collect.CountSpectatorTime`: при `false`
в агрегат уходит `duration_seconds - spectator_seconds`. Поэтому топ по
наигранному можно считать двумя способами, не меняя настройку:

```sql
-- Чистое игровое время за 30 дней, независимо от настройки плагина
SELECT steamid64,
       ROUND(SUM(duration_seconds - COALESCE(spectator_seconds, 0)) / 3600, 1) AS hours
FROM ch_sessions
WHERE ended_at IS NOT NULL AND started_at >= UTC_TIMESTAMP() - INTERVAL 30 DAY
GROUP BY steamid64
ORDER BY hours DESC
LIMIT 50;
```

`NULL` в `spectator_seconds` означает «сессия записана схемой ниже 3-й версии,
тогда не измеряли» — `COALESCE` трактует такие строки как «весь сеанс игровой».

## Расхождения между целями

Три реализации пишут одну схему, но не всё доступно каждой из них одинаково.
Единственный практический вывод для читателя базы: перечисленные ниже колонки
могут быть `NULL` не потому, что «данных не было», а потому, что конкретная
цель их не собирает.

| Колонка | CounterStrikeSharp | SwiftlyS2 | Metamod (нативный) |
|---|---|---|---|
| `kills`, `deaths`, `assists`, `headshots`, `damage`, `mvp` | да | да | да (из игровых событий) |
| `rounds_played`, `team_final`, `team_changes` | да | да | да |
| `score` | да | да | **всегда `NULL`** |
| `ping_avg`, `ping_min`, `ping_max`, `ping_samples` | да | да | **всегда `NULL`** |
| всё остальное | да | да | да |

**Почему в нативной цели нет счёта и пинга.** Оба живут только в полях
контроллера игрока, а чтобы добраться до контроллера, нужен указатель на
`CGameEntitySystem`, добываемый смещением от `GameResourceServiceServer`. Это
единственная константа, которую пришлось бы захардкодить, и ломается она ровно
тогда, когда Valve двигает структуру, — то есть в любое обновление игры. Цена
неверна: убийства, смерти, помощь, урон, MVP и раунды берутся из игровых
событий и такой платы не требуют.

Если отчёт строится по нескольким серверам с разными плагинами, счёт и пинг
стоит считать по `WHERE ping_samples IS NOT NULL`, а не по `> 0`.

### Мелочи, на которые запросы не влияют

* **Длина ника.** C#-цели режут ник до 128 **символов**, нативная — до 128
  **байт** по границе UTF-8. Колонка `VARCHAR(128)` вмещает обе, разница видна
  только на очень длинных никах из кириллицы или эмодзи.
* **`disconnect_reason_name`.** C#-цели пишут человекочитаемое имя причины
  (`DISCONNECT_BY_USER`), нативная — `REASON_<код>`: таблица из полутора сотен
  констант разъехалась бы с игрой на первом обновлении. Сам код в
  `disconnect_reason` одинаков везде и является единственным надёжным
  основанием для группировки.
* **`DisplayTimeZone`.** Нативная цель понимает `UTC`, `Local` и смещение вида
  `+03:00`, но не имена IANA. На данные это не влияет никак: в базе всегда UTC.
* **`ch_online_snapshots.bots`.** У нативной цели всегда `0`: она считает онлайн
  по своему реестру сессий, а ботов туда не заводит (у них нет SteamID). Колонка
  `players` при этом верна у всех трёх — она и так считает только людей. Графики
  посещаемости строятся по `players`, так что на них это не сказывается.

## Готовые запросы

### Кто сейчас на сервере
```sql
SELECT nickname, steamid64, started_at,
       TIMESTAMPDIFF(SECOND, started_at, UTC_TIMESTAMP()) AS online_seconds
FROM ch_sessions
WHERE server_id = 1 AND ended_at IS NULL AND end_kind = 0
ORDER BY started_at;
```

### Топ по наигранному времени
```sql
SELECT last_nickname, steamid64,
       ROUND(total_seconds / 3600, 1) AS hours,
       sessions_count, last_seen
FROM ch_players
ORDER BY total_seconds DESC
LIMIT 50;
```

### Последние заходы конкретного игрока
```sql
SELECT started_at, ended_at, duration_seconds, connect_map,
       kills, deaths, disconnect_reason_name
FROM ch_sessions
WHERE steamid64 = ? AND ended_at IS NOT NULL
ORDER BY started_at DESC
LIMIT 20;
```

### Онлайн по часам суток (когда сервер живой)
```sql
SELECT HOUR(taken_at) AS hour_utc,
       ROUND(AVG(players), 1) AS avg_players,
       MAX(players) AS peak
FROM ch_online_snapshots
WHERE server_id = 1 AND taken_at >= UTC_TIMESTAMP() - INTERVAL 30 DAY
GROUP BY HOUR(taken_at)
ORDER BY hour_utc;
```

### Новые игроки за неделю и сколько из них вернулось (ретеншн)
```sql
SELECT DATE(p.first_seen) AS joined,
       COUNT(*) AS newcomers,
       SUM(p.sessions_count > 1) AS returned
FROM ch_players p
WHERE p.first_seen >= UTC_TIMESTAMP() - INTERVAL 7 DAY
GROUP BY DATE(p.first_seen)
ORDER BY joined;
```

### Популярность карт по наигранному времени
```sql
SELECT connect_map,
       COUNT(*) AS sessions,
       ROUND(SUM(duration_seconds) / 3600, 1) AS hours,
       ROUND(AVG(duration_seconds) / 60, 1) AS avg_minutes
FROM ch_sessions
WHERE ended_at IS NOT NULL AND started_at >= UTC_TIMESTAMP() - INTERVAL 30 DAY
GROUP BY connect_map
ORDER BY hours DESC;
```

### Почему игроки уходят
```sql
SELECT disconnect_reason_name, COUNT(*) AS times
FROM ch_sessions
WHERE ended_at IS NOT NULL AND started_at >= UTC_TIMESTAMP() - INTERVAL 7 DAY
GROUP BY disconnect_reason_name
ORDER BY times DESC;
```

### География аудитории
```sql
SELECT country_iso, COUNT(DISTINCT steamid64) AS players,
       ROUND(SUM(duration_seconds) / 3600) AS hours
FROM ch_sessions
WHERE country_iso IS NOT NULL AND started_at >= UTC_TIMESTAMP() - INTERVAL 30 DAY
GROUP BY country_iso
ORDER BY players DESC;
```

### История смены ников
```sql
SELECT nickname, first_seen, last_seen, times_seen
FROM ch_nicknames
WHERE steamid64 = ?
ORDER BY last_seen DESC;
```

### Возможные мультиаккаунты (по хешу IP, без хранения самого адреса)
```sql
SELECT ip_hash, COUNT(DISTINCT steamid64) AS accounts,
       GROUP_CONCAT(DISTINCT steamid64) AS steam_ids
FROM ch_sessions
WHERE ip_hash IS NOT NULL AND started_at >= UTC_TIMESTAMP() - INTERVAL 90 DAY
GROUP BY ip_hash
HAVING accounts > 1
ORDER BY accounts DESC;
```

### Карта падений сервера
```sql
SELECT DATE(started_at) AS day, connect_map,
       COUNT(*) AS interrupted_sessions
FROM ch_sessions
WHERE end_kind = 5                      -- сервер умер во время сессии
GROUP BY day, connect_map
ORDER BY day DESC, interrupted_sessions DESC;
```

### Качество связи по странам
```sql
SELECT country_iso,
       ROUND(AVG(ping_avg)) AS avg_ping,
       COUNT(*) AS sessions
FROM ch_sessions
WHERE ping_samples > 0 AND started_at >= UTC_TIMESTAMP() - INTERVAL 30 DAY
GROUP BY country_iso
HAVING sessions > 20
ORDER BY avg_ping;
```

### Серверы без публичного адреса
```sql
SELECT id, hostname, last_seen
FROM ch_servers
WHERE address = '';
```
Пусто в `address` — не сбой записи, а «плагин не смог определить адрес».
Лечится настройкой `Server.PublicAddress` в `Settings.json` на этом сервере.

## Права для читающего пользователя

Панели и отчётам запись не нужна:

```sql
CREATE USER 'ch_reader'@'%' IDENTIFIED BY '...';
GRANT SELECT ON connect_history.* TO 'ch_reader'@'%';
```

Самому плагину нужен отдельный пользователь и не больше, чем:

```sql
GRANT SELECT, INSERT, UPDATE, CREATE, INDEX, ALTER ON connect_history.* TO 'ch_plugin'@'%';
```

`DROP` и `DELETE` плагину не нужны никогда — он не удаляет данные.

## Чистка старых данных

Плагин ничего не удаляет сам. Ретеншн — решение владельца сервера:

```sql
-- Убрать персональные данные, оставив аналитику
UPDATE ch_sessions
SET player_ip = NULL, city = NULL
WHERE started_at < UTC_TIMESTAMP() - INTERVAL 90 DAY AND player_ip IS NOT NULL;

-- Проредить снимки онлайна старше полугода до одного в час
DELETE FROM ch_online_snapshots
WHERE taken_at < UTC_TIMESTAMP() - INTERVAL 180 DAY AND MINUTE(taken_at) <> 0;
```
