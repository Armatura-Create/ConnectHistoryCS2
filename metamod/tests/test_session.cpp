// Арифметика сессии. Те же случаи, что в SessionMathTests у C#-целей:
// колонки duration_seconds, spectator_seconds, ping_* и rounds_played читаются
// одними запросами независимо от того, каким плагином они записаны.
#include "core/session.h"

#include "doctest.h"

namespace {

constexpr int64_t kT0 = 1767268800;  // 2026-01-01 12:00:00 UTC
constexpr int64_t kMinute = 60;

ch::OpenSession NewSession() {
    ch::OpenSession session;
    session.key = "0123456789abcdef0123456789abcdef";
    session.steamId64 = 76561198000000001ull;
    session.startedAt = kT0;
    return session;
}

}  // namespace

TEST_CASE("Смена команды на то же значение сменой не считается") {
    ch::OpenSession session = NewSession();
    session.StartTeamTracking(kT0);

    session.NoteTeam(2, kT0);
    CHECK(session.TeamChanges() == 0);

    session.NoteTeam(2, kT0 + kMinute);
    CHECK(session.TeamChanges() == 0);

    session.NoteTeam(3, kT0 + 2 * kMinute);
    CHECK(session.TeamChanges() == 1);
    CHECK(session.LastTeam() == 3);
}

TEST_CASE("Время наблюдателя копится между сменами команды") {
    ch::OpenSession session = NewSession();
    session.StartTeamTracking(kT0);

    session.NoteTeam(ch::OpenSession::kTeamSpectator, kT0);
    session.NoteTeam(3, kT0 + 10 * kMinute);
    session.NoteTeam(ch::OpenSession::kTeamSpectator, kT0 + 40 * kMinute);
    session.FinishTeamTracking(kT0 + 45 * kMinute);

    CHECK(session.SpectatorSeconds() == 15 * 60);
}

TEST_CASE("Последний интервал закрывается на выходе") {
    // Самый вероятный способ потерять учёт: игрок ушёл в спектаторы
    // и просто отключился, смены команды больше не было
    ch::OpenSession session = NewSession();
    session.StartTeamTracking(kT0);

    session.NoteTeam(ch::OpenSession::kTeamSpectator, kT0 + 5 * kMinute);
    CHECK(session.SpectatorSeconds() == 0);  // интервал ещё идёт

    session.FinishTeamTracking(kT0 + 25 * kMinute);
    CHECK(session.SpectatorSeconds() == 20 * 60);
}

TEST_CASE("Время до первого player_team считается игровым") {
    // Сознательно: если событие не придёт вовсе, игрок не должен остаться
    // с нулевым наигранным временем
    ch::OpenSession session = NewSession();
    session.StartTeamTracking(kT0);
    session.FinishTeamTracking(kT0 + 3 * 3600);

    CHECK(session.SpectatorSeconds() == 0);
}

TEST_CASE("Без команды — тоже не игра") {
    ch::OpenSession session = NewSession();
    session.StartTeamTracking(kT0);

    session.NoteTeam(ch::OpenSession::kTeamUnassigned, kT0);
    session.NoteTeam(2, kT0 + 3 * kMinute);
    session.FinishTeamTracking(kT0 + 60 * kMinute);

    CHECK(session.SpectatorSeconds() == 3 * 60);
}

TEST_CASE("Игравший всю сессию не набирает времени вне игры") {
    ch::OpenSession session = NewSession();
    session.StartTeamTracking(kT0);

    session.NoteTeam(2, kT0);
    session.NoteTeam(3, kT0 + 30 * kMinute);
    session.FinishTeamTracking(kT0 + 60 * kMinute);

    CHECK(session.SpectatorSeconds() == 0);
}

TEST_CASE("Пинг: нулевые и абсурдные замеры отбрасываются") {
    ch::OpenSession session = NewSession();

    session.AddPing(0);      // движок отдаёт 0 сразу после входа
    session.AddPing(-1);
    session.AddPing(5000);   // абсурд
    CHECK(session.PingSamples() == 0);
    CHECK(session.PingAvg() == 0);

    session.AddPing(40);
    session.AddPing(60);
    session.AddPing(50);

    CHECK(session.PingSamples() == 3);
    CHECK(session.PingMin() == 40);
    CHECK(session.PingMax() == 60);
    CHECK(session.PingAvg() == 50);
}

TEST_CASE("Раунды считаются в пределах сессии") {
    ch::OpenSession session = NewSession();
    CHECK(session.RoundsPlayed() == 0);

    session.NoteRoundEnd();
    session.NoteRoundEnd();
    CHECK(session.RoundsPlayed() == 2);
}

TEST_CASE("Реестр ключуется по SteamID, а не по слоту") {
    // Массив по слотам ловит и выход за границы, и переиспользование слота
    // движком: новый игрок получил бы время входа предыдущего
    ch::SessionRegistry registry;

    ch::OpenSession first = NewSession();
    first.steamId64 = 76561198000000001ull;
    first.startedAt = kT0;
    registry.Add(first);

    ch::OpenSession second = NewSession();
    second.steamId64 = 76561198000000002ull;
    second.startedAt = kT0 + 1000;
    registry.Add(second);

    CHECK(registry.Count() == 2);

    ch::OpenSession found;
    REQUIRE(registry.TryGet(76561198000000002ull, &found));
    CHECK(found.startedAt == kT0 + 1000);

    ch::OpenSession taken;
    REQUIRE(registry.Take(76561198000000001ull, &taken));
    CHECK(taken.startedAt == kT0);
    CHECK(registry.Count() == 1);

    // Повторный disconnect для того же игрока — обычное дело, не ошибка
    CHECK_FALSE(registry.Take(76561198000000001ull, &taken));

    CHECK(registry.TakeAll().size() == 1);
    CHECK(registry.Count() == 0);
}

TEST_CASE("Update правит сессию на месте") {
    ch::SessionRegistry registry;
    registry.Add(NewSession());

    const bool updated = registry.Update(76561198000000001ull,
                                         [](ch::OpenSession& s) { s.AddPing(70); });
    CHECK(updated);

    ch::OpenSession found;
    REQUIRE(registry.TryGet(76561198000000001ull, &found));
    CHECK(found.PingSamples() == 1);
    CHECK(found.PingAvg() == 70);

    CHECK_FALSE(registry.Update(999ull, [](ch::OpenSession&) {}));
}
