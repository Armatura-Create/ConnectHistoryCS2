// Чтение состояния игры из памяти сервера: контроллеры игроков и gamerules.
//
// ВЫЗЫВАТЬ ТОЛЬКО ИЗ ГЛАВНОГО ПОТОКА: это память движка.
//
// Всё здесь стоит на одном числе — смещении системы сущностей внутри
// IGameResourceService (gamedata.json). Поля дальше читаются по именам через
// ISchemaSystem, то есть по схеме самой игры. Любая функция отвечает false,
// когда прочитать нечем: карта не поднята, смещение не задано или не прошло
// проверку, сущность разобрана, схема не отдала поле. В этом случае значения
// не трогаются — терять статистику дешевле, чем сорвать обработчик.
#pragma once

#include <cstdint>

namespace ch {

struct MatchStats;

namespace stats {

// Итоги матча с контроллера в слоте slot — те же поля, что читает
// StatsCollector в целях на C#: score, MVP, команда и CSMatchStats_t.
// Поля сессии (rounds, teamChanges) не трогает.
bool ReadController(int slot, MatchStats* out);

// Команда игрока в слоте slot (1 — наблюдатель, 2 — T, 3 — CT).
bool ReadTeam(int slot, int32_t* team);

// m_totalRoundsPlayed из gamerules текущей карты.
bool ReadRoundsPlayed(int32_t* rounds);

// Сбросить всё, что принадлежит карте: указатель на gamerules и итог проверки
// смещения. Зовётся при смене карты и перечитывании конфига.
void ForgetMap();

}  // namespace stats
}  // namespace ch
