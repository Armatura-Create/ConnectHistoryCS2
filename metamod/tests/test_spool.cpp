// Спул — единственная защита от потери данных при недоступной базе.
// Круговорот «задание -> строка -> задание» обязан быть без потерь: молча
// потерянное поле обнаружилось бы пустой колонкой месяцы спустя.
#include "core/logger.h"
#include "core/spool.h"

#include "doctest.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TempDir {
public:
    TempDir() {
        char pattern[] = "/tmp/ch_spool_testXXXXXX";
        const char* made = mkdtemp(pattern);
        _path = made != nullptr ? made : "/tmp/ch_spool_fallback";
    }
    ~TempDir() {
        const std::string command = "rm -rf '" + _path + "'";
        if (std::system(command.c_str()) != 0) { /* мусор в /tmp безвреден */ }
    }
    std::string File(const std::string& name) const { return _path + "/" + name; }

private:
    std::string _path;
};

ch::WriteJob FullCloseJob() {
    ch::WriteJob job;
    job.kind = ch::JobKind::SessionClose;
    job.serverId = 7;
    job.sessionKey = "0123456789abcdef0123456789abcdef";
    job.steamId64 = 76561198000000001ull;
    job.accountId = 39734273u;
    job.nickname = "Игрок \"с кавычками\"";
    job.startedAt = 1767268800;
    job.endedAt = 1767272400;
    job.durationSeconds = 3600;
    job.spectatorSeconds = 1800;
    job.countSpectatorTime = false;
    job.disconnectMap = "de_dust2";
    job.disconnectReason = 2;
    job.disconnectReasonName = "DISCONNECT_BY_USER";
    job.endKind = ch::SessionEndKind::Disconnect;
    job.pingAvg = 42;
    job.pingMin = 30;
    job.pingMax = 90;
    job.pingSamples = 12;
    job.stats.valid = true;
    job.stats.kills = 10;
    job.stats.deaths = 7;
    job.stats.assists = 3;
    job.stats.headShots = 4;
    job.stats.damage = 1500;
    job.stats.mvps = 2;
    job.stats.roundsPlayed = 20;
    job.stats.team = 3;
    job.stats.teamChanges = 1;
    return job;
}

}  // namespace

TEST_CASE("Закрытие сессии переживает круговорот через спул") {
    const ch::WriteJob original = FullCloseJob();

    ch::WriteJob restored;
    REQUIRE(ch::DeserializeJob(ch::SerializeJob(original), &restored));

    CHECK(restored.kind == original.kind);
    CHECK(restored.serverId == original.serverId);
    CHECK(restored.sessionKey == original.sessionKey);
    CHECK(restored.steamId64 == original.steamId64);
    CHECK(restored.accountId == original.accountId);
    CHECK(restored.nickname == original.nickname);
    CHECK(restored.startedAt == original.startedAt);
    CHECK(restored.endedAt == original.endedAt);
    CHECK(restored.durationSeconds == original.durationSeconds);
    CHECK(restored.spectatorSeconds == original.spectatorSeconds);
    // Настройка едет ВМЕСТЕ с заданием: она могла измениться, пока задание лежало,
    // а строка обязана попасть в базу по тем правилам, по которым была собрана
    CHECK(restored.countSpectatorTime == false);
    CHECK(restored.disconnectMap == original.disconnectMap);
    CHECK(restored.disconnectReason == original.disconnectReason);
    CHECK(restored.disconnectReasonName == original.disconnectReasonName);
    CHECK(restored.endKind == original.endKind);
    CHECK(restored.pingAvg == original.pingAvg);
    CHECK(restored.pingSamples == original.pingSamples);
    CHECK(restored.stats.valid);
    CHECK(restored.stats.kills == 10);
    CHECK(restored.stats.damage == 1500);
    CHECK(restored.stats.team == 3);
    CHECK_FALSE(restored.stats.hasScore);
}

TEST_CASE("Открытие сессии: отсутствие значения отличается от пустой строки") {
    // NULL в колонке country_iso означает «не определили», а пустая строка —
    // «определили пустоту». Разница видна в любом отчёте по странам
    ch::WriteJob job;
    job.kind = ch::JobKind::SessionOpen;
    job.sessionKey = "k";
    job.steamId64 = 76561198000000001ull;
    job.startedAt = 1767268800;
    job.connectMap = "de_mirage";
    job.hasPlayerIp = true;
    job.playerIp = "203.0.113.7";
    job.hasCountryIso = false;

    ch::WriteJob restored;
    REQUIRE(ch::DeserializeJob(ch::SerializeJob(job), &restored));

    CHECK(restored.hasPlayerIp);
    CHECK(restored.playerIp == "203.0.113.7");
    CHECK_FALSE(restored.hasCountryIso);
    CHECK_FALSE(restored.hasCity);
    CHECK(restored.connectMap == "de_mirage");
}

TEST_CASE("Задание сервера и снимок онлайна переживают круговорот") {
    ch::WriteJob server;
    server.kind = ch::JobKind::ServerUpsert;
    server.serverId = 3;
    server.address = "203.0.113.10:27015";
    server.hostname = "Тестовый сервер";
    server.seenAt = 1767268800;

    ch::WriteJob restoredServer;
    REQUIRE(ch::DeserializeJob(ch::SerializeJob(server), &restoredServer));
    CHECK(restoredServer.address == server.address);
    CHECK(restoredServer.hostname == server.hostname);
    CHECK(restoredServer.seenAt == server.seenAt);

    ch::WriteJob snapshot;
    snapshot.kind = ch::JobKind::OnlineSnapshot;
    snapshot.serverId = 3;
    snapshot.takenAt = 1767268800;
    snapshot.players = 17;
    snapshot.bots = 2;
    snapshot.maxPlayers = 64;
    snapshot.map = "de_nuke";

    ch::WriteJob restoredSnapshot;
    REQUIRE(ch::DeserializeJob(ch::SerializeJob(snapshot), &restoredSnapshot));
    CHECK(restoredSnapshot.players == 17);
    CHECK(restoredSnapshot.bots == 2);
    CHECK(restoredSnapshot.map == "de_nuke");
}

TEST_CASE("Строка спула не содержит переводов строки") {
    // Формат — JSON Lines: перевод строки внутри значения разорвал бы файл
    ch::WriteJob job = FullCloseJob();
    job.nickname = "Игрок\nс переводом";

    const std::string line = ch::SerializeJob(job);
    CHECK(line.find('\n') == std::string::npos);

    ch::WriteJob restored;
    REQUIRE(ch::DeserializeJob(line, &restored));
    CHECK(restored.nickname == job.nickname);
}

TEST_CASE("Битая строка пропускается, а не роняет разбор") {
    ch::WriteJob job;
    CHECK_FALSE(ch::DeserializeJob("не json вовсе", &job));
    CHECK_FALSE(ch::DeserializeJob("{}", &job));
    CHECK_FALSE(ch::DeserializeJob("[1,2,3]", &job));
    // Сессия без ключа бесполезна: по нему идёт UPDATE
    CHECK_FALSE(ch::DeserializeJob("{\"kind\":2,\"steamId64\":1}", &job));
}

TEST_CASE("Файл спула читается целиком и удаляется до отправки") {
    // Иначе повторный сбой записи задвоил бы задания
    TempDir dir;
    ch::NullLogger logger;
    ch::Spool spool(dir.File("pending-writes.jsonl"), 100, &logger);

    CHECK_FALSE(spool.Exists());

    CHECK(spool.Append(FullCloseJob()));
    CHECK(spool.Append(FullCloseJob()));
    CHECK(spool.Exists());

    const std::vector<ch::WriteJob> jobs = spool.TakeAll();
    CHECK(jobs.size() == 2);
    CHECK_FALSE(spool.Exists());
    CHECK(spool.TakeAll().empty());
}

TEST_CASE("Спул не съедает диск") {
    TempDir dir;
    ch::NullLogger logger;
    ch::Spool spool(dir.File("pending-writes.jsonl"), 2, &logger);

    CHECK(spool.Append(FullCloseJob()));
    CHECK(spool.Append(FullCloseJob()));
    // Предел достигнут — задание отбрасывается, но плагин продолжает работать
    CHECK_FALSE(spool.Append(FullCloseJob()));

    CHECK(spool.TakeAll().size() == 2);
}

TEST_CASE("Спул не читается кем попало") {
    // В нём ники и IP игроков
    TempDir dir;
    ch::NullLogger logger;
    const std::string path = dir.File("pending-writes.jsonl");
    ch::Spool spool(path, 100, &logger);
    REQUIRE(spool.Append(FullCloseJob()));

    struct stat info;
    REQUIRE(stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & S_IRGRP) == 0);
    CHECK((info.st_mode & S_IROTH) == 0);
}

TEST_CASE("Счётчик попыток обнуляется при досылке") {
    // Задание из спула — новая попытка, а не продолжение старой: иначе оно
    // мгновенно исчерпает лимит ретраев и уйдёт обратно в спул навсегда
    TempDir dir;
    ch::NullLogger logger;
    ch::Spool spool(dir.File("pending-writes.jsonl"), 100, &logger);

    ch::WriteJob job = FullCloseJob();
    job.attempts = 3;
    REQUIRE(spool.Append(job));

    const std::vector<ch::WriteJob> jobs = spool.TakeAll();
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].attempts == 0);
}
