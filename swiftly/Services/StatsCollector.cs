using System;
using SwiftlyS2.Shared.Players;

namespace ConnectHistory;

/// Снятие игровых метрик с контроллера игрока.
///
/// ВЫЗЫВАТЬ ТОЛЬКО ИЗ ГЛАВНОГО ПОТОКА. Здесь читаются нативы движка; из фонового
/// потока это чтение чужой памяти и мгновенная смерть процесса без стека в логе.
/// Наружу отдаётся POCO-копия — именно она пересекает границу потоков.
public static class StatsCollector
{
    /// Итоги сессии. Любое поле может быть недоступно (сущность уже разбирается),
    /// поэтому чтение защищено: потерять статистику дешевле, чем сорвать обработчик.
    public static MatchStatsSnapshot? Collect(IPlayer? player, OpenSession session, ILogger logger)
    {
        ArgumentNullException.ThrowIfNull(session);

        var snapshot = new MatchStatsSnapshot
        {
            TeamChanges = session.TeamChanges,
            RoundsPlayed = session.RoundsPlayed,
            Team = session.LastTeam < 0 ? 0 : session.LastTeam
        };

        if (player == null) return snapshot;

        try
        {
            var controller = player.Controller;

            snapshot.Score = controller.Score;
            snapshot.Mvps = controller.MVPs;
            snapshot.Team = controller.TeamNum;

            var match = controller.ActionTrackingServices?.MatchStats;
            if (match != null)
            {
                snapshot.Kills = match.Kills;
                snapshot.Deaths = match.Deaths;
                snapshot.Assists = match.Assists;
                snapshot.HeadShots = match.HeadShotKills;
                snapshot.Damage = match.Damage;
            }
        }
        catch (Exception ex)
        {
            // Игрок уже мог стать невалидным — это штатный исход на выходе.
            logger.Debug($"[STATS] не удалось снять итоги сессии {session.SteamId64}: {ex.Message}");
        }

        return snapshot;
    }

    /// Пинг игрока. 0 означает «замер не получился» — такие AddPing отбрасывает сам.
    public static int ReadPing(IPlayer player)
    {
        try
        {
            return (int)player.Controller.Ping;
        }
        catch (Exception)
        {
            return 0;
        }
    }
}
