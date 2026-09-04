# ConnectHistory (CS2)

**English** | [Русский](README.ru.md)

[![CI](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Armatura-Create/ConnectHistoryCS2?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Armatura-Create/ConnectHistoryCS2/total?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases)
[![.NET 10](https://img.shields.io/badge/.NET-10.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![CounterStrikeSharp](https://img.shields.io/badge/CounterStrikeSharp-%E2%89%A5%201.0.369-1f6feb?logo=steam)](https://github.com/roflmuffin/CounterStrikeSharp)
[![MySQL](https://img.shields.io/badge/MySQL-5.7%20%7C%208.0%2B-4479A1?logo=mysql&logoColor=white)](https://www.mysql.com/)
[![Platforms](https://img.shields.io/badge/Platforms-Linux%20%7C%20Windows-2ea44f)](#installation)
[![GeoLite2](https://img.shields.io/badge/GeoLite2-bundled%20%2F%20auto--download-009688)](#build)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue)](LICENSE)

Connection history and player analytics for CounterStrikeSharp / CS2. Every session is
written to MySQL with map, geo, match stats and connection quality — ready for a web panel.

## Features

- **Sessions opened on join, closed on leave.** A crashed server leaves a visible record instead of nothing.
- **"Who is online" is a plain `SELECT`** — no RCON, no A2S queries.
- **Full session data**: SteamID64, nickname, map, disconnect reason, country/city, IP (or a salted hash), kills/deaths/assists/damage/MVP/score, rounds, team, average/min/max ping.
- **Aggregates** — total playtime, session count, first/last seen, nickname history.
- **Online snapshots** for attendance charts.
- **Survives database outages** — records are spooled to disk and re-sent automatically.
- **Never blocks the game thread** — a single background writer, one bounded connection pool.
- **JSON config with a JSON Schema** — your editor autocompletes fields and highlights typos.

## Installation

> Requires **CounterStrikeSharp v1.0.369+** and **MySQL 5.7 / 8.0+ (or MariaDB 10.4+)**.
> v1.0.369 is the first release running on .NET 10, and this plugin targets `net10.0`.

1. Download `ConnectHistory.zip` from the [latest release](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
   and extract it into the root of your game server:

```
addons/counterstrikesharp/plugins/ConnectHistory/
├── ConnectHistory.dll
├── MySqlConnector.dll
├── MaxMind.GeoIP2.dll
├── MaxMind.Db.dll
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```

2. Start the server — the plugin creates its config files and database tables automatically.
3. Fill in the `Database` section of `Settings.json` and run `css_ch_reload`.

`Settings.json` holds the database password, so the plugin keeps it at mode `600`
and never ships it inside the release archive.

## Configuration

`csgo/addons/counterstrikesharp/configs/plugins/ConnectHistory/`

| File | Purpose |
|---|---|
| `Settings.json` | database, what to collect, storage, commands |
| `Messages.json` | texts for player commands, per language |
| `*.schema.json` | JSON Schema, regenerated on every load |
| `README.txt` | short reference, regenerated on every load |

Key options:

```jsonc
{
  "ServerId": 1,              // must differ between servers
  "DisplayTimeZone": "UTC",   // what players see: "Europe/Moscow", "UTC" or "Local".
                              // The database always stores UTC — this never affects it.
  "Database": {
    "Host": "127.0.0.1",
    "SslMode": "Required",    // anything but localhost should use this
    "TablePrefix": "ch_"
  },
  "Collect": {
    "GeoIp": true,
    "PlayerIp": true,         // personal data — turn off if you don't need it
    "IpHash": true,
    "IpHashSalt": "",         // empty salt disables hashing (a saltless hash is reversible)
    "MatchStats": true,
    "Ping": true,
    "OnlineSnapshots": true
  }
}
```

## Commands

| Command | Permission | Action |
|---|---|---|
| `css_ch_status` | `@css/root` | database connectivity, queue size, last error |
| `css_ch_reload` | `@css/root` | reload configuration |
| `css_playtime` | any player | own playtime and session count |
| `css_lastseen` | any player | own recent sessions |

## Database

Six tables, all created by the plugin: `ch_sessions`, `ch_players`, `ch_nicknames`,
`ch_servers`, `ch_online_snapshots`, `ch_schema_version`.

Full schema and a query cookbook: [docs/DATABASE.md](docs/DATABASE.md).
Building a panel module: [docs/FLUTE_MODULE.md](docs/FLUTE_MODULE.md).

Give the plugin its own MySQL user — it never needs `DROP` or `DELETE`:

```sql
GRANT SELECT, INSERT, UPDATE, CREATE, INDEX, ALTER ON connect_history.* TO 'ch_plugin'@'%';
```

## Build

```bash
export PATH="$HOME/.dotnet:$PATH"
./build.sh            # restore, build, test, package into bin/Release/net10.0/ConnectHistory.zip
```

Or manually:

```bash
dotnet build -c Release      # also packages the zip
dotnet test                  # xUnit
```

MySQL integration tests are skipped unless `CH_TEST_MYSQL` is set:

```bash
docker run --rm -d -p 3399:3306 -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=ch mysql:8
export CH_TEST_MYSQL="server=127.0.0.1;port=3399;user=root;password=test;database=ch"
dotnet test
```

## Credits

GeoLite2 data by [MaxMind](https://www.maxmind.com).
