// Итоги матча с контроллера игрока — то же, что читает StatsCollector
// в целях на C#: score, MVP, команда и CSMatchStats_t.
//
// ВЫЗЫВАТЬ ТОЛЬКО ИЗ ГЛАВНОГО ПОТОКА: это память движка.
#pragma once

namespace ch {

struct MatchStats;

namespace stats {

// Заполняет kills/deaths/assists/headShots/damage/mvps/team и score из
// контроллера в слоте slot. Поля сессии (rounds, teamChanges) не трогает.
//
// false — прочитать нечем: нет Utils, нет системы сущностей (карта ещё не
// поднята), контроллер уже разобран или схема не отдала смещение. В этом
// случае out остаётся как был — терять статистику дешевле, чем сорвать
// обработчик отключения.
bool ReadController(int slot, MatchStats* out);

}  // namespace stats
}  // namespace ch
