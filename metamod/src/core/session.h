// Открытая сессия игрока и реестр таких сессий.
//
// Состояние ключуется по SteamID64, а НЕ по номеру слота: массив по слотам ловит
// и выход за границы, и переиспользование слота движком — новый игрок получил бы
// время входа предыдущего.
//
// Указатель на игрока здесь не хранится: за время сессии объект будет освобождён
// движком, и обращение к нему станет чтением чужой памяти.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ch {

class OpenSession {
public:
    std::string key;
    uint64_t steamId64 = 0;
    uint32_t accountId = 0;
    int64_t startedAt = 0;
    std::string connectMap;

    // Ник обновляется: игрок может сменить его в течение сессии.
    std::string nickname;
    std::string countryIso;

    // Номера команд в CS2: 0 — не выбрана, 1 — наблюдатель, 2 — T, 3 — CT.
    static constexpr int32_t kTeamUnassigned = 0;
    static constexpr int32_t kTeamSpectator = 1;

    int32_t PingSamples() const { return _pingSamples; }
    int32_t PingMin() const { return _pingMin; }
    int32_t PingMax() const { return _pingMax; }
    // Среднее по всем снятым замерам. 0, если замеров не было.
    int32_t PingAvg() const {
        return _pingSamples == 0 ? 0 : static_cast<int32_t>(_pingSum / _pingSamples);
    }

    int32_t LastTeam() const { return _lastTeam; }
    int32_t TeamChanges() const { return _teamChanges; }
    int32_t RoundsPlayed() const { return _roundsPlayed; }

    // Секунды, проведённые в наблюдателях и без команды.
    //
    // Копится по событиям смены команды: длительность сессии — это время
    // подключения, а «наиграно» — время в составе команды. Разделить их
    // постфактум нельзя, в базе остаётся только итоговая команда.
    int32_t SpectatorSeconds() const { return _spectatorSeconds; }

    // Отсчёт времени команды начинается вместе с сессией.
    void StartTeamTracking(int64_t startedAtSeconds) { _teamSince = startedAtSeconds; }

    // Нулевой и абсурдный пинг игнорируем: движок отдаёт 0 в первые секунды после
    // входа и на смене карты, и такие замеры занижали бы среднее.
    void AddPing(int32_t ping);

    // Сыграно раундов. Считаем событием round_end, а не чтением схемы движка:
    // поля контроллера обнуляются сменой карты, а нам нужно ровно то,
    // что игрок застал в ЭТОЙ сессии.
    void NoteRoundEnd() { ++_roundsPlayed; }

    // Первая увиденная команда — это не смена, а начальное состояние.
    // now передаётся аргументом, а не берётся внутри: так метод остаётся
    // проверяемым тестом без ожиданий в реальном времени.
    void NoteTeam(int32_t team, int64_t now);

    // Закрывает последний интервал команды. Зовётся при закрытии сессии,
    // иначе время после последней смены команды нигде не учтётся.
    void FinishTeamTracking(int64_t now) { AccrueTeamTime(now); }

private:
    // Относит истёкший интервал к «вне игры», если игрок провёл его наблюдателем
    // или без команды.
    //
    // Состояние ДО первого события player_team (lastTeam = -1) намеренно считается
    // игровым: если событие почему-то не придёт вовсе, игрок не должен остаться
    // с нулевым наигранным временем. Ошибаться безопаснее в сторону прежнего поведения.
    void AccrueTeamTime(int64_t now);

    int32_t _pingSamples = 0;
    int32_t _pingMin = 0;
    int32_t _pingMax = 0;
    int64_t _pingSum = 0;

    int32_t _lastTeam = -1;
    int32_t _teamChanges = 0;
    int32_t _roundsPlayed = 0;
    int32_t _spectatorSeconds = 0;

    // Момент, с которого длится текущее состояние команды. 0 — ещё не начинали.
    int64_t _teamSince = 0;
};

// Реестр открытых сессий. Обращаются игровые события и колбэк таймера пинга —
// всё в главном потоке, но мьютекс оставлен сознательно: цена нулевая,
// а инвариант «структура не рвётся» держится независимо от будущих изменений.
class SessionRegistry {
public:
    size_t Count() const;

    void Add(OpenSession session);

    // false, если сессии нет.
    bool TryGet(uint64_t steamId, OpenSession* out) const;

    // Обновляет уже лежащую в реестре сессию на месте. Возвращает false,
    // если её там нет.
    template <typename Fn>
    bool Update(uint64_t steamId, Fn&& fn) {
        std::lock_guard<std::mutex> guard(_mutex);
        auto it = _sessions.find(steamId);
        if (it == _sessions.end()) return false;
        fn(it->second);
        return true;
    }

    template <typename Fn>
    void ForEach(Fn&& fn) {
        std::lock_guard<std::mutex> guard(_mutex);
        for (auto& pair : _sessions) fn(pair.second);
    }

    // Забирает сессию из реестра. false, если её там нет: disconnect может прийти
    // для игрока, которого мы не открывали (бот, ранний выход).
    bool Take(uint64_t steamId, OpenSession* out);

    // Забирает все сессии разом (смена карты, выгрузка плагина).
    std::vector<OpenSession> TakeAll();

private:
    mutable std::mutex _mutex;
    std::map<uint64_t, OpenSession> _sessions;
};

}  // namespace ch
