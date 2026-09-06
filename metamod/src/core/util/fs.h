// Файловая система: ровно то, чего не хватает и конфигу, и спулу.
#pragma once

#include <string>

namespace ch {
namespace fs {

bool DirectoryExists(const std::string& path);

// Создаёт каталог ВМЕСТЕ С РОДИТЕЛЯМИ.
//
// Отдельная функция, а не mkdir по месту: одиночный mkdir полного пути падает
// с ENOENT, если промежуточного каталога нет, и оба места в плагине на этом уже
// обожглись — конфиги не создавались, спул не писался. Обе поломки выглядели
// как что-то совсем другое.
bool EnsureDirectory(const std::string& path);

// Каталог, в котором лежит файл. Пусто, если разделителя в пути нет.
std::string ParentDirectory(const std::string& path);

}  // namespace fs
}  // namespace ch
