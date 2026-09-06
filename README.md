# ConnectHistory (CS2)

**English** | [Русский](README.ru.md)

[![CI CSSharp](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-cssharp.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-cssharp.yml)
[![CI SwiftlyS2](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-swiftly.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-swiftly.yml)
[![CI Metamod](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-metamod.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-metamod.yml)
[![Release](https://img.shields.io/github/v/release/Armatura-Create/ConnectHistoryCS2?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Armatura-Create/ConnectHistoryCS2/total?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases)
[![.NET 10](https://img.shields.io/badge/.NET-10.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![CounterStrikeSharp](https://img.shields.io/badge/CounterStrikeSharp-%E2%89%A5%201.0.369-1f6feb?logo=steam)](https://github.com/roflmuffin/CounterStrikeSharp)
[![SwiftlyS2](https://img.shields.io/badge/SwiftlyS2-%E2%89%A5%201.4.9-8957e5)](https://github.com/swiftly-solution/swiftlys2)
[![Metamod:Source](https://img.shields.io/badge/Metamod%3ASource-%E2%89%A5%202.0.0--git1322-f0883e)](https://www.sourcemm.net/downloads.php?branch=master)
[![MySQL](https://img.shields.io/badge/MySQL-5.7%20%7C%208.0%2B-4479A1?logo=mysql&logoColor=white)](https://www.mysql.com/)
[![Platforms](https://img.shields.io/badge/Platforms-Linux%20%7C%20Windows-2ea44f)](#installation)
[![GeoLite2](https://img.shields.io/badge/GeoLite2-bundled%20%2F%20auto--download-009688)](#build)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue)](LICENSE)

Connection history and player analytics for CS2. Every session is written to MySQL
with map, geo, match stats and connection quality — ready for a web panel.

**Three implementations, three platforms, one database schema.** The plugin exists
separately for CounterStrikeSharp, SwiftlyS2 and native Metamod:Source. They share
no code — they share the schema, and all three write the same tables. You can run
different plugins on different servers and build a single report.

| Platform | Requirements | Archive | Installs into |
|---|---|---|---|
| **CounterStrikeSharp** | CSSharp ≥ 1.0.369 (hence Metamod:Source) | `ConnectHistory_cssharp_<version>.zip` | `addons/counterstrikesharp/plugins/ConnectHistory/` |
| **SwiftlyS2** | SwiftlyS2 ≥ 1.4.9, Metamod **not needed** | `ConnectHistory_swiftly_<version>.zip` | `addons/swiftlys2/plugins/ConnectHistory/` |
| **Metamod:Source** | Metamod:Source ≥ 2.0.0-git1322, nothing else | `ConnectHistory_metamod_<version>.zip` | `addons/ConnectHistory/` |

The native target records the connection history in full but collects no match
results — what is missing and why is in
[Differences between targets](docs/DATABASE.md#differences-between-targets).

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

> Requires **MySQL 5.7 / 8.0+ (or MariaDB 10.4+)**. Everything else depends on the
> platform — see the table above.

1. Download the archive for your platform from the
   [latest release](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
   and extract it into the root of your game server. The directory layout is already
   inside, together with the GeoLite2 databases.

<details>
<summary>CounterStrikeSharp</summary>

```
addons/counterstrikesharp/plugins/ConnectHistory/
├── ConnectHistory.dll
├── MySqlConnector.dll
├── MaxMind.GeoIP2.dll
├── MaxMind.Db.dll
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```
</details>

<details>
<summary>SwiftlyS2</summary>

```
addons/swiftlys2/plugins/ConnectHistory/
├── ConnectHistory.dll
├── MySqlConnector.dll
├── MaxMind.GeoIP2.dll
├── MaxMind.Db.dll
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```
</details>

<details>
<summary>Metamod:Source</summary>

Windows and Linux ship in the same archive — the unused directory is simply ignored.

```
addons/metamod/ConnectHistory.vdf
addons/ConnectHistory/
├── bin/win64/ConnectHistory.dll
├── bin/linuxsteamrt64/ConnectHistory.so
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```
</details>

2. Start the server — the plugin creates its config files and database tables automatically.
3. Fill in the `Database` section of `Settings.json` and reload the configuration
   (`css_ch_reload`, `sw_ch_reload` or `ch_reload`, depending on the platform).

`Settings.json` holds the database password, so the plugin keeps it at mode `600`
and never ships it inside the release archive.

## Configuration

| Platform | Config directory |
|---|---|
| CounterStrikeSharp | `csgo/addons/counterstrikesharp/configs/plugins/ConnectHistory/` |
| SwiftlyS2 | `csgo/addons/swiftlys2/configs/plugins/ConnectHistory/` |
| Metamod:Source | `csgo/addons/ConnectHistory/configs/` |

The file format is identical: `Settings.json` and `Messages.json` move between
platforms unchanged.

| File | Purpose |
|---|---|
| `Settings.json` | database, what to collect, storage, commands |
| `Messages.json` | texts for player commands, per language |
| `*.schema.json` | JSON Schema, regenerated on every load |
| `README.txt` | short reference, regenerated on every load |

Key options:

```jsonc
{
  "DisplayTimeZone": "UTC",   // what players see: "Europe/Moscow", "UTC" or "Local".
                              // The database always stores UTC — this never affects it.
  "Server": {
    "Id": 1,                  // must differ between servers. Was a top-level "ServerId"
                              // before 3.0.1; the old spelling is still read.
    "PublicAddress": ""       // "ip:port" or "host:port". Empty means auto-detect, but
                              // the server process does not know its own public address:
                              // ConVar ip returns the socket bind address, usually 0.0.0.0.
  },
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
    "CountSpectatorTime": true,  // false — do not count spectator and
                                // unassigned time as playtime
    "OnlineSnapshots": true
  }
}
```

## Commands

| Action | CounterStrikeSharp | SwiftlyS2 | Metamod |
|---|---|---|---|
| connectivity, queue, last error | `css_ch_status` (`@css/root`) | `sw_ch_status` | `ch_status` (server console) |
| reload configuration | `css_ch_reload` (`@css/root`) | `sw_ch_reload` | `ch_reload` (server console) |
| own playtime | `css_playtime` (player) | `sw_playtime` (player) | `ch_playtime <steamid64>` (console) |
| recent sessions | `css_lastseen` (player) | `sw_lastseen` (player) | `ch_lastseen <steamid64>` (console) |

In the native target the player commands are console commands: sending a message
to a specific player goes through a protobuf UserMessage, and an "almost working"
command that prints the answer in the wrong place is worse than an honest console one.

## Database

Six tables, all created by the plugin: `ch_sessions`, `ch_players`, `ch_nicknames`,
`ch_servers`, `ch_online_snapshots`, `ch_schema_version`.

Full schema and a query cookbook: [docs/DATABASE.md](docs/DATABASE.md).

A ready-made section for the Flute CMS panel:
**[ConnectHistory-Flute](https://github.com/Armatura-Create/ConnectHistory-Flute)** —
online list, statistics, filters and charts on top of these tables. Installation and
wiring are documented in that repository.

Give the plugin its own MySQL user — it never needs `DROP` or `DELETE`:

```sql
GRANT SELECT, INSERT, UPDATE, CREATE, INDEX, ALTER ON connect_history.* TO 'ch_plugin'@'%';
```

## Build

Each target builds on its own.

```bash
export PATH="$HOME/.dotnet:$PATH"

cd cssharp && ./build.sh      # tests + archive in bin/Release/net10.0/
cd swiftly && ./build.sh      # same
```

MySQL integration tests are skipped unless `CH_TEST_MYSQL` is set:

```bash
docker run --rm -d -p 3399:3306 -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=ch mysql:8
export CH_TEST_MYSQL="server=127.0.0.1;port=3399;user=root;password=test;database=ch"
dotnet test cssharp/ConnectHistory.sln
```

The native target is split in two. Its core knows nothing about hl2sdk or MySQL
and is verified on any machine in a second:

```bash
cd metamod && make -f Makefile.tests -j8 && ./build-tests/ch_tests
```

Building the plugin itself needs hl2sdk, Metamod:Source and a static MariaDB
client — that lives in CI. Details: [metamod/README.md](metamod/README.md).

## Credits

GeoLite2 data by [MaxMind](https://www.maxmind.com), distributed under the
[GeoLite2 End User License Agreement](https://www.maxmind.com/en/geolite2/eula).

## License

[GPL-3.0-or-later](LICENSE).
