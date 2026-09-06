# Changelog

The section matching the release tag becomes the body of the GitHub release, so keep
entries short and grouped. Add the section **before** tagging.

## 3.0.0

### Added
- **SwiftlyS2 target.** A separate plugin for SwiftlyS2 servers, released as
  `ConnectHistory_swiftly_<version>.zip`. SwiftlyS2 needs no Metamod — it has its own
  loader via `gameinfo.gi`.
- **Native Metamod:Source target.** A C++ plugin for servers running neither
  CounterStrikeSharp nor SwiftlyS2, released as `ConnectHistory_metamod_<version>.zip`
  with Windows and Linux binaries in one archive.
- Startup banner in the server console with the plugin name, version, platform and
  server number — on all three platforms.
- `CHANGELOG.md`, and release notes generated from it.

### Fixed
- **`spectator_seconds` was never written when a session closed normally.** The column
  was missing from the `UPDATE`, so every normally closed session left it `NULL`. The
  `ch_players.total_seconds` aggregate was correct all along — only the per-session
  history was lost. Existing rows are not backfilled; new sessions are correct.

### Changed
- **Repository restructured into `cssharp/`, `swiftly/` and `metamod/`.** The three
  implementations share no code — only the database schema, which is now an explicit
  contract in `docs/DATABASE.md`.
- **Release archives are renamed** to `ConnectHistory_<platform>_<version>.zip`. Direct
  links to `ConnectHistory.zip` will no longer resolve; use the release page.
- Documentation is now in English by default (`README.ru.md` remains the Russian
  translation). `docs/FLUTE_MODULE.md` was removed — the panel module documents itself at
  [ConnectHistory-Flute](https://github.com/Armatura-Create/ConnectHistory-Flute).
- CI is split per target with path filters; the release workflow builds all three from a
  single tag.

### Known limitations of the Metamod target
- `score` and `ping_*` are always `NULL`, and `ch_online_snapshots.bots` is always `0`.
  Reaching those values needs a hardcoded engine offset that breaks on game updates.
- `ch_playtime` and `ch_lastseen` are server-console commands taking a SteamID64, not
  in-game player commands.
- See [Differences between targets](docs/DATABASE.md#differences-between-targets).

## 2.2.0 and earlier

See the [release history](https://github.com/Armatura-Create/ConnectHistoryCS2/releases).
