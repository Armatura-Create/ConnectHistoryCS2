namespace ConnectHistory;

/// Преобразования SteamID.
public static class SteamIdUtil
{
    /// Steam account ID (младшие 32 бита SteamID64) — то, что исторически лежало
    /// в колонке account_id.
    ///
    /// Возвращаем uint, а не int: аккаунты давно перевалили за 2^31, и в колонке
    /// signed int такие ID превращались в отрицательные числа. В БД колонка INT UNSIGNED.
    public static uint ToAccountId(ulong steamId64) => (uint)(steamId64 & 0xFFFFFFFFUL);

    /// SteamID64 валиден, если это индивидуальный аккаунт Steam (universe 1, type 1).
    /// Боты и невалидные контроллеры отдают 0 — такие записи в историю не попадают.
    public static bool IsRealSteamId(ulong steamId64) => steamId64 > 76561197960265728UL;
}
