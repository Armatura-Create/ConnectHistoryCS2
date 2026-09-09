// Смещения полей сущностей по именам — через ISchemaSystem движка.
//
// Это НЕ gamedata. Схема — часть самой игры: движок публикует раскладку своих
// классов по именам, и смещение m_iScore в CCSPlayerController спрашивается
// у него в рантайме. Обновление игры двигает поле — движок отвечает новым числом.
// Захардкоженных констант здесь нет.
#pragma once

#include <cstdint>

namespace ch {
namespace schema {

// Смещение поля от начала объекта класса серверного модуля.
// -1 — класс или поле не найдены (один раз уходит в лог), кэшируется.
int32_t FieldOffset(const char* className, const char* fieldName);

}  // namespace schema
}  // namespace ch
