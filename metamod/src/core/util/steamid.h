// Преобразования SteamID. Поведение обязано совпадать с SteamIdUtil в C#-целях:
// колонки account_id и steamid64 читаются одними и теми же запросами из docs/DATABASE.md.
#pragma once

#include <cstdint>

namespace ch {

// Steam account ID — младшие 32 бита SteamID64. В базе колонка INT UNSIGNED:
// аккаунты давно перевалили за 2^31, и в signed int такие id уходили в минус.
inline uint32_t ToAccountId(uint64_t steamId64) {
    return static_cast<uint32_t>(steamId64 & 0xFFFFFFFFull);
}

// SteamID64 валиден, если это индивидуальный аккаунт Steam.
// Боты и невалидные контроллеры отдают 0 — такие записи в историю не попадают.
inline bool IsRealSteamId(uint64_t steamId64) {
    return steamId64 > 76561197960265728ull;
}

}  // namespace ch
