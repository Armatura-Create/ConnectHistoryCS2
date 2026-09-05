// Время. В базу всё уходит в UTC — это соглашение держит вся история отчётов
// по часам. Игроку время показывается в поясе DisplayTimeZone, и это единственное
// место, где UTC покидает данные.
#pragma once

#include <cstdint>
#include <string>

namespace ch {

class ILogger;

// Секунды эпохи Unix, UTC. Единственный источник времени в плагине:
// локальное время в базу не уходит нигде.
int64_t UtcNowSeconds();

// "YYYY-MM-DD HH:MM:SS" в UTC — формат MySQL DATETIME.
std::string FormatSqlDateTime(int64_t epochSeconds);

// Смещение пояса отображения в секундах.
//
// Поддерживаются "UTC", "Local" и фиксированное смещение "+03:00" / "-0500".
// Имена IANA ("Europe/Moscow") здесь НЕ поддержаны сознательно: в C++ для них
// нужна либо база tzdata через C++20 <chrono> (её нет в MSVC-сборке под Windows
// без дополнительных файлов), либо правка глобальной переменной TZ процесса —
// а она общая на весь игровой сервер и не потокобезопасна. Имя IANA приводит
// к явной ошибке в логе и падению на Local: сервер физически стоит в своём поясе,
// и это ближе к намерению, чем молчаливый сдвиг на UTC.
//
// На то, что пишется в базу, настройка не влияет вообще.
int32_t ResolveDisplayOffsetSeconds(const std::string& zone, ILogger* logger = nullptr);

// "YYYY-MM-DD HH:MM" в поясе отображения. Формат совпадает с C#-целями.
std::string FormatDisplayDateTime(int64_t epochSeconds, int32_t offsetSeconds);

// Человекочитаемая длительность: "3h 12m" вместо 11520 секунд.
// Формат совпадает с ChatFormat.Duration в C#-целях.
std::string FormatDuration(int64_t seconds);

}  // namespace ch
