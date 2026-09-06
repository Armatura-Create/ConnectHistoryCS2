/* libmaxminddb, собранная одним файлом, плюс открытие базы ИЗ ПАМЯТИ.
 *
 * Зачем нужно открытие из памяти. Штатный MMDB_open умеет только mmap
 * (POSIX) и CreateFileMapping (Windows), а внутри игрового процесса это
 * фатально: том Docker/overlayfs или движок со своими обработчиками сигналов
 * превращают страничный отказ в SIGBUS, и процесс умирает мгновенно — без
 * исключения, без стека, без строки в логе. В C#-целях ровно поэтому стоит
 * FileAccessMode.Memory; здесь эквивалента в библиотеке нет, и его приходится
 * добавлять.
 *
 * Почему через #include самого maxminddb.c: всё, что делает MMDB_open после
 * отображения файла, опирается на статические функции библиотеки
 * (find_metadata, read_metadata, find_ipv4_start_node). Включение
 * трансляционной единицы целиком даёт к ним доступ, не трогая сабмодуль:
 * обновление вендора остаётся обычным `git submodule update`.
 *
 * Цена режима — оперативная память размером с базу: Country ~9 МБ, City ~60 МБ.
 *
 * ВАЖНО при обновлении libmaxminddb: тело ch_mmdb_open_memory повторяет
 * последовательность инициализации из MMDB_open. Если апстрим её изменит,
 * компилятор промолчит. Сверять при каждом подъёме версии сабмодуля.
 */
/* maxminddb.c sets this before it includes anything, and glibc latches its
 * feature-test macros on the FIRST system header it sees. We include our own
 * header before maxminddb.c, so the macro has to be set here - otherwise
 * <netdb.h> arrives without struct addrinfo and <limits.h> without SSIZE_MAX,
 * and the vendored file fails to compile. Apple libc and the SteamRT clang do
 * not gate on this, which is why it only showed up on gcc/glibc. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "core/geoip_memory.h"

/* MMDB_lib_version() отдаёт эту строку. Обычно её подставляет autotools/CMake;
   мы собираем библиотеку одним файлом, поэтому берём версию сабмодуля явно.
   Разъедется при обновлении вендора — поднять здесь. */
#define PACKAGE_VERSION "1.13.3"

/* Сама библиотека */
#include "maxminddb.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ch_mmdb_open_memory(const char *const filename, MMDB_s *const mmdb) {
    int status = MMDB_SUCCESS;
    uint8_t *buffer = NULL;
    long size = 0;
    FILE *file = NULL;

    mmdb->file_content = NULL;
    mmdb->data_section = NULL;
    mmdb->metadata.database_type = NULL;
    mmdb->metadata.languages.count = 0;
    mmdb->metadata.languages.names = NULL;
    mmdb->metadata.description.count = 0;

    mmdb->filename = mmdb_strdup(filename);
    if (NULL == mmdb->filename) {
        return MMDB_OUT_OF_MEMORY_ERROR;
    }

    /* Обычный ввод-вывод: ошибка чтения — это errno, а не сигнал */
    file = fopen(filename, "rb");
    if (NULL == file) {
        status = MMDB_FILE_OPEN_ERROR;
        goto cleanup;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        status = MMDB_IO_ERROR;
        goto cleanup;
    }

    size = ftell(file);
    if (size <= 0) {
        status = MMDB_IO_ERROR;
        goto cleanup;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        status = MMDB_IO_ERROR;
        goto cleanup;
    }

    buffer = (uint8_t *)malloc((size_t)size);
    if (NULL == buffer) {
        status = MMDB_OUT_OF_MEMORY_ERROR;
        goto cleanup;
    }

    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        status = MMDB_IO_ERROR;
        goto cleanup;
    }

    fclose(file);
    file = NULL;

    mmdb->file_content = buffer;
    mmdb->file_size = (ssize_t)size;
    buffer = NULL; /* владение перешло в mmdb */

    /* Дальше — та же последовательность, что в MMDB_open после map_file */
    {
        uint32_t metadata_size = 0;
        const uint8_t *metadata =
            find_metadata(mmdb->file_content, mmdb->file_size, &metadata_size);
        if (NULL == metadata) {
            status = MMDB_INVALID_METADATA_ERROR;
            goto cleanup;
        }

        mmdb->metadata_section = metadata;
        mmdb->metadata_section_size = metadata_size;

        status = read_metadata(mmdb);
        if (MMDB_DECODER_LIMIT_ERROR == status) {
            status = MMDB_INVALID_METADATA_ERROR;
        }
        if (MMDB_SUCCESS != status) {
            goto cleanup;
        }

        if (mmdb->metadata.binary_format_major_version != 2) {
            status = MMDB_UNKNOWN_DATABASE_FORMAT_ERROR;
            goto cleanup;
        }

        if (!can_multiply(SSIZE_MAX, mmdb->metadata.node_count,
                          mmdb->full_record_byte_size)) {
            status = MMDB_INVALID_METADATA_ERROR;
            goto cleanup;
        }

        {
            ssize_t search_tree_size = (ssize_t)mmdb->metadata.node_count *
                                       (ssize_t)mmdb->full_record_byte_size;
            ssize_t data_section_size = 0;

            mmdb->data_section = mmdb->file_content + search_tree_size +
                                 MMDB_DATA_SECTION_SEPARATOR;
            if (mmdb->file_size < MMDB_DATA_SECTION_SEPARATOR ||
                search_tree_size > mmdb->file_size - MMDB_DATA_SECTION_SEPARATOR) {
                status = MMDB_INVALID_METADATA_ERROR;
                goto cleanup;
            }

            data_section_size =
                mmdb->file_size - search_tree_size - MMDB_DATA_SECTION_SEPARATOR;
            if (data_section_size > UINT32_MAX || data_section_size <= 0) {
                status = MMDB_INVALID_METADATA_ERROR;
                goto cleanup;
            }
            mmdb->data_section_size = (uint32_t)data_section_size;

            if (mmdb->data_section_size < 3) {
                status = MMDB_INVALID_DATA_ERROR;
                goto cleanup;
            }
        }

        mmdb->metadata_section = metadata;
        mmdb->ipv4_start_node.node_value = 0;
        mmdb->ipv4_start_node.netmask = 0;

        if (mmdb->metadata.ip_version == 6) {
            status = find_ipv4_start_node(mmdb);
            if (status != MMDB_SUCCESS) {
                goto cleanup;
            }
        }
    }

cleanup:
    if (NULL != file) {
        fclose(file);
    }
    if (NULL != buffer) {
        free(buffer);
    }
    if (MMDB_SUCCESS != status) {
        ch_mmdb_close_memory(mmdb);
    }
    return status;
}

void ch_mmdb_close_memory(MMDB_s *const mmdb) {
    /* MMDB_close зовёт munmap на file_content — нашему буферу это смертельно.
     * Прячем указатель, даём библиотеке освободить своё (filename, метаданные),
     * и освобождаем буфер сами. */
    uint8_t *owned = (uint8_t *)mmdb->file_content;

    mmdb->file_content = NULL;
    mmdb->file_size = 0;
    mmdb->data_section = NULL;
    mmdb->data_section_size = 0;
    mmdb->metadata_section = NULL;
    mmdb->metadata_section_size = 0;

    MMDB_close(mmdb);

    if (NULL != owned) {
        free(owned);
    }
}
