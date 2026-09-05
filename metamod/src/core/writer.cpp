#include "core/writer.h"

#include "core/logger.h"
#include "core/util/sql_sanitizer.h"
#include "core/util/timeutil.h"

#include <algorithm>
#include <chrono>

namespace ch {
namespace {

// Досылку спула не делаем чаще раза в минуту: база могла восстановиться,
// но долбиться в неё на каждой успешной записи незачем.
constexpr int64_t kSpoolReplayIntervalSeconds = 60;

// Остановка не должна вешать сервер. Пять секунд — столько же, сколько ждёт
// C#-версия; остаток уходит в спул и досылается при следующем старте.
constexpr int kStopTimeoutMilliseconds = 5000;

}  // namespace

SessionWriter::SessionWriter(IDatabase* database, const StorageSettings& storage,
                             const std::string& spoolDirectory, ILogger* logger)
    : _database(database),
      _storage(storage),
      _logger(logger),
      _spool(spoolDirectory.empty() ? std::string("pending-writes.jsonl")
                                    : spoolDirectory + "/pending-writes.jsonl",
             storage.spoolMaxEntries, logger) {
    _thread = std::thread(&SessionWriter::Run, this);
}

SessionWriter::~SessionWriter() { Stop(); }

void SessionWriter::Enqueue(const WriteJob& job) {
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stopping) {
            // Очередь закрыта — задание не теряем, кладём сразу в спул
            SpoolJob(job);
            return;
        }
        _queue.push_back(job);
    }
    _signal.notify_one();
}

size_t SessionWriter::Pending() const {
    std::lock_guard<std::mutex> guard(_mutex);
    return _queue.size();
}

std::string SessionWriter::LastError() const {
    std::lock_guard<std::mutex> guard(_errorMutex);
    return _lastError;
}

void SessionWriter::Run() {
    // Спул с прошлого запуска досылаем первым делом: сервер мог упасть
    // с непустой очередью.
    ReplaySpool();

    for (;;) {
        WriteJob job;
        {
            std::unique_lock<std::mutex> guard(_mutex);
            _signal.wait(guard, [this] { return _stopping || !_queue.empty(); });

            if (_queue.empty()) {
                if (_stopping) return;
                continue;
            }

            job = _queue.front();
            _queue.pop_front();
        }

        Process(job);
    }
}

void SessionWriter::Process(WriteJob job) {
    const int attempts = std::max(1, _storage.retryAttempts);
    const int delaySeconds = std::max(1, _storage.retryDelaySeconds);

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        job.attempts = attempt;

        std::string error;
        if (Write(job, &error)) {
            _written.fetch_add(1);
            {
                std::lock_guard<std::mutex> guard(_errorMutex);
                _lastError.clear();
            }

            // База отвечает — самое время досыпать отложенное
            const int64_t now = UtcNowSeconds();
            if (_spool.Exists() && now - _lastSpoolReplay >= kSpoolReplayIntervalSeconds) {
                ReplaySpool();
            }
            return;
        }

        {
            std::lock_guard<std::mutex> guard(_errorMutex);
            _lastError = job.Describe() + ": " + MaskSecrets(error);
        }

        if (attempt == attempts) {
            if (_logger != nullptr) {
                _logger->Error("[DB] Не удалось записать " + job.Describe() + " за " +
                               std::to_string(attempts) +
                               " попыт(ок). Задание уходит в спул и будет дослано позже: " +
                               MaskSecrets(error));
            }
            SpoolJob(job);
            return;
        }

        // Пауза растёт с номером попытки: база могла перезагружаться
        std::unique_lock<std::mutex> guard(_mutex);
        if (_signal.wait_for(guard, std::chrono::seconds(delaySeconds * attempt),
                             [this] { return _stopping; })) {
            guard.unlock();
            SpoolJob(job);
            return;
        }
    }
}

bool SessionWriter::Write(const WriteJob& job, std::string* error) {
    const std::string prefix = _database->Prefix();

    switch (job.kind) {
        case JobKind::ServerUpsert:
            return _database->Execute(BuildServerUpsert(prefix, job), nullptr, error);

        case JobKind::OnlineSnapshot:
            return _database->Execute(BuildOnlineSnapshot(prefix, job), nullptr, error);

        case JobKind::SessionOpen: {
            for (const Statement& statement : BuildSessionOpen(prefix, job)) {
                if (!_database->Execute(statement, nullptr, error)) return false;
            }
            return true;
        }

        case JobKind::SessionClose:
            return WriteClose(job, error);
    }

    *error = "неизвестный тип задания";
    return false;
}

// Закрытие сессии идемпотентно.
//
// UPDATE ставит условие ended_at IS NULL. Если он ничего не изменил — сессия уже
// закрыта (задание пришло повторно из спула) или строки открытия вообще нет.
// Решение принимает PlanClose, и именно оно проверяется тестом: повтор задания
// не имеет права удвоить наигранное время.
bool SessionWriter::WriteClose(const WriteJob& job, std::string* error) {
    const std::string prefix = _database->Prefix();

    if (!_database->Begin(error)) return false;

    int affected = 0;
    if (!_database->Execute(BuildSessionCloseUpdate(prefix, job), &affected, error)) {
        _database->Rollback();
        return false;
    }

    bool rowExists = false;
    if (affected == 0) {
        std::vector<ResultRow> rows;
        if (!_database->Query(BuildSessionExists(prefix, job), &rows, error)) {
            _database->Rollback();
            return false;
        }
        rowExists = !rows.empty();
    }

    const CloseAction action = PlanClose(affected, rowExists);

    if (action == CloseAction::AlreadyClosed) {
        // Счётчики не трогаем — иначе повтор задания удвоит наигранное время
        return _database->Commit(error);
    }

    if (action == CloseAction::InsertThenAggregates) {
        if (!_database->Execute(BuildSessionCloseInsert(prefix, job), nullptr, error)) {
            _database->Rollback();
            return false;
        }
    }

    for (const Statement& statement : BuildAggregates(prefix, job)) {
        if (!_database->Execute(statement, nullptr, error)) {
            _database->Rollback();
            return false;
        }
    }

    return _database->Commit(error);
}

void SessionWriter::SpoolJob(const WriteJob& job) {
    if (!_storage.spoolEnabled) {
        if (_logger != nullptr) {
            _logger->Error("[DB] Спул выключен — задание потеряно: " + job.Describe());
        }
        return;
    }

    if (_spool.Append(job)) _spooled.fetch_add(1);
}

void SessionWriter::ReplaySpool() {
    _lastSpoolReplay = UtcNowSeconds();

    const std::vector<WriteJob> jobs = _spool.TakeAll();
    if (jobs.empty()) return;

    if (_logger != nullptr) {
        _logger->Info("[DB] Досылаю " + std::to_string(jobs.size()) + " отложенных записей");
    }

    for (const WriteJob& job : jobs) {
        std::string error;
        if (!Write(job, &error)) {
            // Не прошло снова — обратно в спул обычным путём, без задвоения
            SpoolJob(job);
            continue;
        }
        _written.fetch_add(1);
    }
}

void SessionWriter::Stop() {
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stopping) return;
        _stopping = true;
    }
    _signal.notify_all();

    if (_thread.joinable()) {
        // Поток сам выйдет, разобрав очередь; ждать бесконечно нельзя
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kStopTimeoutMilliseconds);

        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> guard(_mutex);
                if (_queue.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        _thread.join();
    }

    // Всё, что не успело, кладём в спул
    std::deque<WriteJob> remaining;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        remaining.swap(_queue);
    }

    if (!remaining.empty() && _logger != nullptr) {
        _logger->Warn("[DB] Писатель не успел разобрать очередь — остаток (" +
                      std::to_string(remaining.size()) + ") уходит в спул");
    }

    for (const WriteJob& job : remaining) SpoolJob(job);
}

}  // namespace ch
