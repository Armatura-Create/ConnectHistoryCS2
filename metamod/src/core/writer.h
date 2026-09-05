// Единственное место, где плагин пишет в базу.
//
// Главный поток кладёт готовый снимок в очередь и немедленно возвращается в кадр —
// ни одно обращение к нативам движка сюда не попадает и попасть не может: через
// границу проходят только POCO-копии значений (WriteJob).
//
// Писатель ОДИН. Не «по потоку на игрока»: 64 игрока на смене карты — это
// 64 параллельных подключения к MySQL из игрового процесса.
#pragma once

#include "core/config.h"
#include "core/database.h"
#include "core/jobs.h"
#include "core/spool.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace ch {

class ILogger;

class SessionWriter {
public:
    SessionWriter(IDatabase* database, const StorageSettings& storage,
                  const std::string& spoolDirectory, ILogger* logger);
    ~SessionWriter();

    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;

    // Вызывается из главного потока. Никогда не блокирует и никогда не бросает:
    // проблема с базой не имеет права сорвать обработчик игрового события.
    void Enqueue(const WriteJob& job);

    // Наблюдаемое состояние для ch_status
    size_t Pending() const;
    int64_t Written() const { return _written.load(); }
    int64_t Spooled() const { return _spooled.load(); }
    std::string LastError() const;
    bool SpoolExists() const { return _spool.Exists(); }

    // Даём писателю доработать очередь, а всё, что не успело, кладём в спул.
    // Ждать бесконечно нельзя: выгрузка плагина не должна вешать сервер.
    void Stop();

private:
    void Run();
    void Process(WriteJob job);
    bool Write(const WriteJob& job, std::string* error);
    bool WriteClose(const WriteJob& job, std::string* error);
    void SpoolJob(const WriteJob& job);
    void ReplaySpool();

    IDatabase* _database;
    StorageSettings _storage;
    ILogger* _logger;
    Spool _spool;

    mutable std::mutex _mutex;
    std::condition_variable _signal;
    std::deque<WriteJob> _queue;
    bool _stopping = false;

    std::atomic<int64_t> _written{0};
    std::atomic<int64_t> _spooled{0};

    mutable std::mutex _errorMutex;
    std::string _lastError;

    int64_t _lastSpoolReplay = 0;

    std::thread _thread;
};

}  // namespace ch
