# CLAUDE.md

**ConnectHistory** — connection history and player analytics for CS2, stored in MySQL,
implemented **three independent times**:

| Directory | Platform | Language | Needs Metamod? |
|---|---|---|---|
| `cssharp/` | CounterStrikeSharp ≥ 1.0.369 | C# / net10.0 | yes (CSSharp is itself an MM:S plugin) |
| `swiftly/` | SwiftlyS2 ≥ 1.4.9 | C# / net10.0 | **no**, own loader via `gameinfo.gi` |
| `metamod/` | Metamod:Source | C++17 | yes — this *is* the native MM:S plugin |

**Code is duplicated across targets on purpose** (owner's decision: the targets must
evolve independently). Exactly one thing is shared:

> **`docs/DATABASE.md` is a contract.** All three implementations must write the same
> schema, and the queries in that document must behave identically no matter which
> plugin wrote the row. Any divergence goes into its "Differences between targets"
> section — a silent divergence is worse than a missing feature, because a report
> spanning several servers will quietly come out wrong.

Also shared across the repository: `GeoIP/` (one copy of the GeoLite2 databases),
`docs/`, `LICENSE`, `README*.md`.

**Rule for changing an invariant: change it in all three targets or in none.**
No compiler will remind you — only this file will.

Repository: <https://github.com/Armatura-Create/ConnectHistoryCS2>, licence GPL-3.0-or-later.

Documentation and commit messages are written in **English**. `README.ru.md` is the one
deliberate exception — it is the Russian translation of the user-facing README.

## Target `cssharp/` — CounterStrikeSharp

- `net10.0`, namespace `ConnectHistory`, `[MinimumApiVersion(369)]`
- Dependencies: `CounterStrikeSharp.API` `1.0.369` (pinned, not `*`), `MySqlConnector`
  `2.6.2`, `MaxMind.GeoIP2` `5.3.0`
- **Build against the MINIMUM supported CSSharp version, not the newest.** Then the
  compiler itself proves that no API from a newer build is used, and the plugin loads on
  any server running 1.0.369+. The package version in `.csproj` and `[MinimumApiVersion]`
  must match — `ApiVersionTests` enforces it. Real incident in a sibling plugin: with
  `MinimumApiVersion(373)`, a server on 1.0.371 refused to load the plugin even though
  nothing from 372–373 was used.
- `net10.0` is a requirement, not a preference: CSSharp runs on .NET 10 from v1.0.369.
- The SDK lives in `~/.dotnet` and is not on `PATH` by default:
  `export PATH="$HOME/.dotnet:$PATH"`

## Target `swiftly/` — SwiftlyS2

- `net10.0`, same namespace, `SwiftlyS2.CS2` pinned in `swiftly/Swiftly.props`
- SwiftlyS2 is **not a layer on top of Metamod**: it is wired in with
  `Game csgo/addons/swiftlys2` in `gameinfo.gi`. It is an alternative loader, not an addon.
- Same minimum-version rule: `MinimumAPIVersion` in `PluginMetadata` must equal the package
  version (`ApiVersionTests`). The version lives in `Swiftly.props` because both the plugin
  and the test project need it, and two drifting literals would turn that test from an
  invariant check into an attentiveness check.
- **`PluginMetadata` is an attribute**, and attribute arguments must be compile-time
  constants. MSBuild generates `PluginVersion.g.cs` (target `GeneratePluginVersion`) from
  `<Version>`, so the "never bump a version by hand" rule survives.
- **`SwiftlyS2.CS2.dll` is x64-only** (a CS2 dedicated server is never anything else). It
  will not load on an arm64 machine, so tests that mention its types are skipped via
  `SwiftlyRuntime.Available`. `Skip.IfNot` at the top of the test body does **not** help:
  the JIT resolves every type a method mentions before the first line runs. Such calls live
  in nested `Bound` classes, and pure helpers live in `PluginText`. CI (x64) skips nothing.
- The config directory arrives whole from `Core.Configuration.BasePath`; the spool lives in
  `Core.PluginDataDirectory` (the plugin directory is overwritten on update).

## Target `metamod/` — native C++

- C++17. The core in `src/core` **knows nothing about hl2sdk or MySQL**.
- **No signatures, no offsets, no vtable lookups — in this plugin.** Everything it does
  by itself comes from Metamod hooks and engine interfaces obtained by factory, so a game
  update does not break it. Connection history, ping, chat and commands need nothing else.
- **Match results come through the Utils plugin by Pisex** (`https://github.com/Pisex/cs2-menus`,
  interface `"IUtilsApi"`, looked up in `AllPluginsLoaded`). The controller's fields
  (`m_iScore`, `m_iMVPs`, `m_iTeamNum`, `m_pActionTrackingServices->m_matchStats`) are read
  at disconnect on the main thread, exactly like C# `StatsCollector`; `round_end` and
  `player_team` are the only events hooked. The pointer to the entity system needs gamedata
  that no factory provides; Utils owns it for a whole family of plugins, so ConnectHistory
  borrows it instead of maintaining a second copy. The dependency is **optional** — without
  Utils those columns stay `0`/`NULL` and the console says so once.
- **`src/mm/utils_api.h` mirrors a foreign vtable.** It must repeat `include/menus.h` from
  cs2-menus method by method up to `ClearAllHooks`, and must have no virtual destructor —
  the original has none, and adding one shifts every entry by one. Re-check it when bumping
  the documented Utils version.
- **Field offsets are asked from `ISchemaSystem` by name** (`src/mm/schema.cpp`), walking
  single-inheritance bases. That is the game's own schema, not gamedata: a game update
  moves a field and the engine answers with the new number.
- **Hooks are KHook, not SourceHook.** Metamod:Source removed SourceHook on 2026-09-08
  (`a12f3cd5`) and bumped the plugin API to 18 (`0cc4e200`), so this target needs
  Metamod:Source **2.0.0-git1460 or newer** and does not load on older builds. Hooks are
  `KHook::Virtual<>` members bound to methods in the constructor and attached in `Load`
  (`Add`) / detached in `Unload` (`Remove`); handlers return `KHook::Return<T>` with
  `Action::Ignore` or `Action::Supersede`.
- **Chat and ping need no offsets.** The reply goes out as a `CUserMessageSayText2`
  allocated by `INetworkMessageInternal` and posted through `IGameEventSystem`; the chat is
  heard by hooking `ICvar::DispatchConCommand` (`say` / `say_team`); `ping_*` comes from
  `IVEngineServer2::GetPlayerNetInfo` → `INetChannelInfo::GetAvgLatency`. The UserMessage
  lives alone in `src/mm/chat.cpp`. Ping is sampled **only** from `Hook_GameFrame` —
  `INetChannelInfo` is engine memory, and reading it from the background thread is reading
  someone else's memory.
- **Only our own chat commands are swallowed** (`Action::Supersede`). A history plugin that
  eats every `say` breaks the chat plugins installed next to it, and nothing would point
  at us as the cause.
- **`.mmdb` is opened from memory only** (`ch_mmdb_open_memory`): `libmaxminddb` offers
  nothing but `mmap`, and a page fault inside a game process is a SIGBUS that kills the
  server with no stack. The wrapper `#include`s `maxminddb.c` whole so the submodule stays
  untouched; when bumping the vendor, re-check the initialisation sequence — the compiler
  will not warn if upstream changes it.
- `DisplayTimeZone` understands `UTC`, `Local` and an offset like `+03:00`; IANA names are
  not supported (they need tzdata, or mutating the process-wide `TZ`).
- Vendored as submodules: `nlohmann/json`, `doctest`, `libmaxminddb`, `mariadb-connector-c`
  (LGPL-2.1 — `libmysqlclient` is deliberately avoided, its
  GPL-2.0-with-FOSS-exception conflicts with GPL-3).
- `AMBuildScript` and `configure.py` come from CS2Fixes (GPL-3.0) and are adapted.

## Commands

```bash
export PATH="$HOME/.dotnet:$PATH"

cd cssharp && ./build.sh          # restore -> tests -> Release -> ConnectHistory_cssharp_<version>.zip
cd swiftly && ./build.sh 3.0.0    # same, with an explicit version

dotnet test cssharp/ConnectHistory.sln
dotnet test swiftly/ConnectHistory.sln

# Native core: no hl2sdk, no MySQL, one second on any machine
cd metamod && make -f Makefile.tests -j8 && ./build-tests/ch_tests
```

MySQL integration tests are skipped until `CH_TEST_MYSQL` is set — without it they
silently prove nothing:

```bash
docker run --rm -d -p 3399:3306 -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=ch mysql:8
export CH_TEST_MYSQL="server=127.0.0.1;port=3399;user=root;password=test;database=ch"
```

Building the MM:S plugin itself needs hl2sdk, Metamod:Source and a static MariaDB client —
that lives in CI (`metamod/README.md` also documents the local Linux path).

## CI/CD

One workflow per target, each filtered by path (a C++ build takes minutes, and running it
for a change in `swiftly/` is pointless):

- `ci-cssharp.yml`, `ci-swiftly.yml` — restore, build, test against MySQL 8
- `ci-metamod.yml` — two jobs: `core` (core tests with a plain compiler, seconds) and
  `build` (hl2sdk + Metamod + static MariaDB in a SteamRT3 container and on windows-latest)

`release.yml` fires on tag `v*` (or manual dispatch). The version is computed **once** in
the `version` job and handed to every target: three plugins built from one tag must report
one number. MSBuild stamps it into the C# targets; a generated `src/mm/version.h` stamps
the native one.

The release body is generated from the commits — see [Releases](#releases).

Downside of path filters: a PR that does not touch a target produces no status for it, so
such a check must never be made required in branch protection.

The Linux MM:S binary is built **only** in the Steam Runtime 3 container: that is the ABI a
CS2 dedicated server uses, and a binary from a plain ubuntu runner will not load.

Analyzers (`EnableNETAnalyzers` + `AnalysisMode=Recommended`) are always on and the build
holds at zero warnings; `CA1716` and `CA1859` are silenced deliberately in `.csproj`. The
C++ core builds with `-Wall -Wextra -Wpedantic -Werror`.

Red tests mean no release.

## The one architectural decision

**The session row is created when the player JOINS (`INSERT`), not when they leave.**

The obvious design is to write the row in the disconnect handler, where the outcome is
already known. But a game server process dies without warning, and everything that lived
only in memory dies with it: a crash would mean the session never happened.

What the swap buys:

- `ended_at IS NULL AND end_kind = 0` — **who is on the server right now**, a plain
  `SELECT`, no RCON and no A2S.
- `end_kind = 5` (stale) — **a map of server crashes**: marked at plugin start for sessions
  left open by the previous run. No separate monitoring needed.
- Timeouts, engine kicks and process death now lose the outcome, not the fact of the visit.

The row key is `session_key` (a GUID generated by the plugin), **not** the auto-increment
`id`: writes are asynchronous, and the database id may not be known yet when the player
leaves.

## Architecture

### C# targets (`cssharp/`, `swiftly/`)

Both use the same file layout so they can be compared by eye. `ConnectHistory` is a
`sealed partial class` spread across files:

| File | Contents |
|---|---|
| `ConnectHistory.cs` / `ConnectHistoryPlugin.cs` | `Load`/`Unload`, service wiring, timers, version resolution |
| `Events/ConnectHistory.Events.cs` | the single handler registration point + `SafeEvent` |
| `Events/ConnectHistory.PlayerEvents.cs` | opening/closing sessions, rounds, team changes |
| `Commands/ConnectHistory.Commands.cs` | `*_ch_status`, `*_ch_reload`, `*_playtime`, `*_lastseen` |
| `Services/ConfigService*.cs` | JSON loading, defaults, JSON Schema |
| `Services/DatabaseService.cs` | connection string, pool, `PingAsync` |
| `Services/SchemaService.cs` | DDL, migrations, marking stale sessions |
| `Services/SessionWriter.cs` | queue, retries, disk spool — **the only place that writes to the database** |
| `Services/SessionService.cs` | registry of open sessions (`OpenSession`) |
| `Services/StatsCollector.cs` | reading metrics off the controller (main thread only) |
| `Services/QueryService.cs` | reads for the player commands |
| `Services/GeoIpService.cs` | MaxMind, `FileAccessMode.Memory` |
| `Utils/` | logger, `IpUtil`, `SteamIdUtil`, `SqlSanitizer`, `ChatFormat`, `TimeZoneResolver`, `Banner` |

`swiftly/` adds `Utils/PluginText.cs` — pure helpers (nickname truncation, version parsing).
They live outside the plugin class for a concrete reason: a type deriving from `BasePlugin`
drags in the x64-only SwiftlyS2 assembly, and on arm64 they could not be tested at all.

There is no DI container: services are constructed by hand in `Load()`. Services do not
derive from `BasePlugin` and therefore have no access to timers — those live in the main
plugin file. Do not drag `BasePlugin` into `Services/`.

### Native target (`metamod/`)

The split runs along exactly one line: **does this code know about the engine?**

| Directory | Contents | Verified |
|---|---|---|
| `src/core/` | config, SQL text, sessions, writer, spool, GeoIP, reads, banner | locally, `make -f Makefile.tests` |
| `src/db/` | MariaDB client | CI only |
| `src/mm/` | `ISmmPlugin`, Metamod hooks, console commands | CI only |
| `tests/` | doctest over `src/core` | locally |

This is not aesthetics: hl2sdk does not build on macOS at all, and almost all of the
plugin's logic needs no engine. The split means a hundred tests run in a second on any
machine instead of only on a live server.

`IDatabase` is the only interface in the core, and it earns its place: without it the
writer — with its retries, spool and idempotent session close — could only be verified
against a production database. The test double fails on command.

### Data path (C# targets; the native one is the same with different events)

```
main thread                              │ background thread
EventPlayerConnectFull                   │
  → OpenSessionFor                       │
      GeoIpService.Lookup (IP → country) │
      SessionService.Add(OpenSession)    │
      SessionWriter.Enqueue(OpenJob) ────┼──→ Channel → SessionWriter.RunAsync
                                         │       → INSERT IGNORE + player upsert
ping timer → OpenSession.AddPing         │       → on failure: retries → JSONL spool
EventRoundEnd → OpenSession.NoteRoundEnd │
EventPlayerTeam → OpenSession.NoteTeam   │
EventPlayerDisconnect                    │
  → StatsCollector.Collect (natives!)    │
  → SessionWriter.Enqueue(CloseJob) ─────┼──→ transaction: UPDATE session
                                         │       + ch_players + ch_nicknames
```

Only **POCO copies of values** cross the thread boundary (`WriteJob` and its kin). No
controller, no `ConVar`, nothing from the engine ever goes there.

## Invariants that are easy to break

Written in the vocabulary of the C# targets, but **all three obey them** — the native one
just spells the names differently (`ch::SessionWriter`, `ch_mmdb_open_memory`, and so on).
Change an invariant, change it everywhere.

- **No engine calls from a background thread.** `Utilities.*`, `ConVar.*`, `Server.*`,
  `CCSPlayerController` — main thread only. A memory violation in the native layer kills the
  process with no exception and no stack, which is why the server address and player metrics
  are read inside the event handler and only finished copies travel to the background.
- **No engine calls in `Load()` or in service constructors.** At that point the engine has
  not raised its globals yet, and any `Server.*` / `ConVar.Find` throws
  `NativeException: Global Variables not initialized yet` — the plugin then fails to load
  entirely. That is why `RegisterServer` is deferred by `AddTimer(3.0f, …)`.
- **Get a player ONLY through `Utilities.GetPlayers()` or from the event.**
  `Utilities.GetPlayerFromSlot(slot)` internally does
  `new CCSPlayerController(EntitySystem.GetEntityByIndex(slot + 1))` **without checking the
  entity type**: for a freed or reused index it returns somebody else's entity, and reading
  its fields walks the wrong offsets — the server dies without a single console line.
  `IsValid` does not save you: it checks the pointer, not the type. There must be no
  `GetPlayerFromSlot` call left in the plugin.
- **Session state is keyed by `SteamID`, never by slot number.** A slot-indexed array catches
  both out-of-range access and slot reuse by the engine: a new player would inherit the
  previous one's join time. Covered by `SessionMathTests`.
- **A controller must not cross a frame boundary.** Inside `Task.Run` / `Server.NextFrame`,
  capture the `SteamID` and look the player up again (`FindPlayer`). In that gap the player
  can leave, the object is freed, and even touching `IsValid` becomes a read of foreign memory.
- **`SessionWriter` is the only place that writes to the database, and there is exactly one
  writer.** Do not spawn a `Task.Run` per player: 64 players on a map change means 64
  parallel MySQL connections from the game process.
- **Closing a session must be idempotent.** `UPDATE … WHERE session_key = ? AND
  ended_at IS NULL`; if 0 rows were affected the session is already closed (the job came
  back from the spool) and the counters must not be touched, or playtime doubles. If no row
  exists at all, insert a complete one. Covered by an integration test.
- **The connection string is built ONLY by `MySqlConnectionStringBuilder`.** A password
  containing `;` in an interpolated string substitutes connection parameters (`SslMode=None`
  and anything else). `DatabaseConnectionTests` checks this with a password full of special
  characters. The native target sidesteps the problem structurally: `mysql_real_connect`
  takes separate arguments, so no such string exists.
- **`duration_seconds` is always the connected time.** `Collect.CountSpectatorTime` affects
  only the `ch_players.total_seconds` aggregate. Out-of-game time goes to `spectator_seconds`
  ALWAYS, regardless of the setting: on changing their mind the owner recomputes the
  aggregate instead of losing history.
- **The state before the first `player_team` counts as in-game.** If the event never arrives,
  a player must not end up with zero playtime: erring toward the previous behaviour is safer.
- **The table prefix goes through a whitelist** (`SanitizePrefix`): SQL identifiers cannot be
  parameterised and the value comes from a config file. The regex anchor is `\z`, not `$`:
  in .NET `$` matches before a trailing newline, so `"ch_\n"` would sail through.
- **The public server address comes from the config, not from a ConVar.** The process does
  not know its external address: `ConVar ip` is the socket bind address, normally `0.0.0.0`.
  The source is `Server.PublicAddress`; auto-detection is a fallback whose result is rejected
  for bind, loopback and private addresses (`IpUtil.ResolvePublicAddress`). A filled-in but
  unusable setting is **not** silently replaced by auto-detection: otherwise the human's typo
  goes unnoticed and the database receives an address they never wrote. Covered by
  `ServerAddressTests`.
- **An empty address does not overwrite a stored one** (`SessionWriter.ServerUpsertSql`:
  `address = IF(VALUES(address) = '', address, VALUES(address))`). The address used to be
  rewritten on every start, so a manually corrected row reverted to garbage on the next
  restart. The same rule applies to `hostname`.
- **Secrets never reach the log.** `PluginLogger.Error` runs the exception text through
  `SqlSanitizer.Mask`: MySqlConnector is happy to attach the whole connection string.
- **MaxMind databases are opened with `FileAccessMode.Memory` ONLY.** The default
  `MemoryMapped` reads `.mmdb` through page faults; inside a game process on overlayfs that
  becomes a SIGBUS and kills the process instantly — no exception, no stack, no log line.
  The cost of `Memory` is RAM the size of the database (Country ~9 MB, City ~60 MB).
- **Time is `DateTime.UtcNow` everywhere.** A server with a local time zone would otherwise
  write mixed timestamps and every hourly report would lie. The convention is not held by a
  comment: the connection string carries `DateTimeKind=Utc`, so writing a `Kind=Local` value
  is a driver exception rather than a quiet shift of a few hours. Players see time in the
  `DisplayTimeZone` zone (`TimeZoneResolver`), and that is the only place UTC leaves the data.
  `DateTime.Now` is allowed in exactly two places — the log line stamp and the last-error text
  for `ch_status`; it never reaches the database.
- **Chat messages go out only through `ChatFormat.EnsureChatColorPrefix`.** The CS2 engine
  eats a colour code sitting at the very start of a message, so `{GREEN}[History] …` comes out
  white. The tempting early return — "already starts with a colour code, return as is" —
  disables the fix in exactly the case that needs it; that is how it broke in NotifyMessages.
  Covered by `ChatColorTests`.
- **Numbers are formatted with `CultureInfo.InvariantCulture`.** Without it a server with an
  Arabic or Turkish locale shows players different digits (`IpAndFormatTests`).
- **Nicknames are truncated to 128.** A nickname longer than the column brings down the whole
  `INSERT` with "Data too long" and the session is lost entirely. Nickname, IP and language
  are untrusted input.
- **Event handlers are wrapped in `SafeEvent`.** An exception in our handler must not bubble
  into the framework and disturb other plugins on the server.
- **Open sessions are closed in `Unload`, on map change and in `ch_reload`.** Otherwise they
  hang in the database and "who is online" lies until the next start. Every new exit path
  must call `CloseAllSessions`.
- **A map change is a session boundary.** `Listeners.OnMapStart` closes everything as
  `MapChange`; players open new sessions with their own `player_connect_full`. Without this,
  time smears across several maps and per-map analytics becomes meaningless.
- **Dictionaries keyed by SteamID are cleaned in the disconnect handler**
  (`_commandCooldown`), or they grow for the whole lifetime of the server.
- **Rounds are counted from the `round_end` event, not read off the engine schema**: the
  controller's fields are reset by a map change, and we need what the player saw in THIS session.

## Configuration

Two files: `Settings.json` and `Messages.json`, plus `*.schema.json` and `README.txt` next to
them. The latter two are **rewritten on every load**: while the README was written on first
run only, it described the previous version after every update.

Config directories differ per target — see the table in `README.md`. The file format is
identical, so both files move between platforms unchanged.

A broken file **does not crash the plugin and is not overwritten**: the loader catches the
parse error, logs the file, line and position, and falls back to defaults. Trailing commas
and `//` comments are allowed deliberately — they are the most common "mistakes" in a
hand-edited config. Covered by `ConfigResilienceTests`. Overwriting a broken config would
destroy a human's work along with their typo.

`Settings.json` contains a password, so the plugin sets mode `600` on it and **never ships
it in the release archive** — the release workflow fails if it finds one.

**Adding a setting is four edits:** field in the model → line in the merge step → value in
the defaults → property in the schema. Skipping the merge gives a silent runtime default;
skipping the schema gives a field the editor will say nothing about.

## Database schema

Six tables with the configured prefix: `sessions`, `players`, `nicknames`, `servers`,
`online_snapshots`, `schema_version`. The DDL lives in `SchemaService.BuildSchema`
(`ch::BuildSchema` in `core/schema_sql.cpp`) and must stay idempotent — the plugin gets
reloaded on a live server.

**The DDL text is identical across all three targets, character for character.** That is the
contract: a schema created by one plugin must fit the other two without migration.

There are deliberately no foreign keys: they would tie history to aggregates and make it
impossible to clean old sessions without cascades.

Changing the schema means bumping the version and adding a migration step — **in all three
targets at once**: `SchemaService.CurrentVersion` in C# and `ch::kSchemaVersion` in C++.
`SchemaTests` (C#) and `test_sql.cpp` (C++) check that the version does not lag behind the
migration ladder and that every step is reachable.

Current version is **3**. Step 2 cleans `ch_servers.address` of bind addresses (`0.0.0.0…`)
written by version 1 — an `UPDATE`, not a `DELETE`, because one field is broken, not the
server row. Step 3 adds `ch_sessions.spectator_seconds`. Both are covered by integration
tests against a live MySQL.

**A migration step must survive being applied twice.** `ALTER` has no `IF NOT EXISTS` form in
MySQL (that is MariaDB), and on a fresh database the column already arrives from
`BuildSchema`, so errors 1050/1060/1061 ("object already exists") are treated as "step
applied" (`ApplyMigrationAsync`, `IsAlreadyAppliedError`). Conditional DDL through
`SET @variable` does not work: MySqlConnector treats `@name` as a parameter placeholder and
the query never reaches the server.

The full column contract and a query cookbook live in `docs/DATABASE.md`, which doubles as
documentation for anyone reading the database from outside.

**Divergences between targets are recorded there**, in the "Differences between targets"
section. There are four right now, all consequences of the "no signatures, no offsets" rule
in the native target. A silent divergence is worse than a missing feature.

## Logging

`ILogger` → `PluginLogger`, format `[timestamp] [ConnectHistory] [LEVEL] msg`.
`Debug(...)` prints only when `Config.Debug == true` (off by default — Debug prints
SteamIDs, nicknames and player IPs). The flag is read through a closure, so it takes effect
right after a config reload.

`ILogger.Raw` exists for exactly one consumer: the startup banner, whose frame a per-line
prefix would tear apart.

Do not call `Console.WriteLine` directly. In `cssharp/` it remains for historical reasons; in
`swiftly/` logging goes through `Core.Logger` (and through `[LoggerMessage]`, or the analyzers
rightly complain); in `metamod/` through `META_CONPRINTF`.

The banner frame is computed rather than padded with hardcoded spaces: the version comes from
the release tag and can be longer than expected, and a broken frame in the server console is
the first thing an owner sees about this plugin. Covered by `BannerTests` / `test_banner.cpp`.

## Versions

**Never bump a version by hand.** The single source is the release tag; the `version` job in
`release.yml` computes it once and hands it to every target.

- `cssharp/`: `ModuleVersion` is resolved from `AssemblyInformationalVersionAttribute` of the
  built assembly (`ResolveModuleVersion` / `FormatModuleVersion`), with the `+sha` tail cut
  off. Covered by `ModuleVersionTests`.
- `swiftly/`: the same, plus `PluginVersion.g.cs` generated by MSBuild from `<Version>` —
  the `PluginMetadata` attribute needs a compile-time constant.
- `metamod/`: CI rewrites `src/mm/version.h` from the tag; the repository holds a
  `0.0.0-dev` fallback.

## Releases

**There is no `CHANGELOG.md`.** The `publish` job builds the release body from the
commits between the previous `v*` tag and the new one, grouped by Conventional Commit
type. A hand-maintained file is a file somebody forgets, and then a release ships with
the previous version's description; commits cannot be forgotten.

The consequence: **a commit subject is user-facing text.** `CONTRIBUTING.md` holds the
format. A commit that does not match it is not dropped — it lands under "Other", because
silently losing a change from the notes is worse than showing it uncategorised.

The `publish` job therefore needs `fetch-depth: 0`: without the tags there is nothing to
diff against, and the notes would silently cover the whole history.

The metamod target ships **two archives**, one per OS
(`ConnectHistory_metamod_{linux,windows}_<version>.zip`). One archive for both weighed
twice what anyone needed, and half of it could not run on the machine it was unpacked on.

## Code graph

`graphify-out/` holds a generated code graph (`graph.json`, `GRAPH_REPORT.md`). Questions
like "what calls X" or "how are Y and Z related" are faster through `graphify query "..."`
than by re-reading files. After noticeable changes: `graphify <path> --update`.

The current graph was built **before** the repository was split into three targets and knows
nothing about the `cssharp/`, `swiftly/` and `metamod/` paths — rebuild it before relying on it.
