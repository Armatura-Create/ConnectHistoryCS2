// Записывает файлы конфигурации по умолчанию в указанный каталог.
//
// Нужен для сборки релиза: архив обязан содержать configs/, потому что каталог
// сервера может быть доступен только на чтение, а плагин без Settings.json
// работает вхолостую.
//
// Умышленно вызывает ConfigService::LoadOrCreate, а не пишет файлы сам: так
// в архив попадает ровно то, что создал бы плагин на первом запуске. Отдельный
// генератор рано или поздно разъехался бы с настоящим, и разошлись бы они молча.
#include "core/config.h"
#include "core/logger.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: dump_defaults <directory>\n");
        return 2;
    }

    ch::NullLogger logger;
    ch::ConfigService service(&logger);
    service.LoadOrCreate(argv[1]);

    if (service.Directory().empty()) {
        std::fprintf(stderr, "dump_defaults: could not write into %s\n", argv[1]);
        return 1;
    }

    std::printf("default configs written to %s\n", argv[1]);
    return 0;
}
