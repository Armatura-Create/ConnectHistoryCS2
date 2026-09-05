/* Открытие базы MaxMind ИЗ ПАМЯТИ вместо mmap.
 *
 * mmap внутри игрового процесса превращает любой сбой чтения страницы в SIGBUS
 * и убивает сервер без единой строки в логе. Реальный инцидент, ради которого
 * это написано, произошёл при полностью целом файле — на overlayfs в Docker.
 */
#ifndef CH_GEOIP_MEMORY_H
#define CH_GEOIP_MEMORY_H

#include "maxminddb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MMDB_SUCCESS или код ошибки libmaxminddb. */
int ch_mmdb_open_memory(const char *const filename, MMDB_s *const mmdb);

/* Освобождает то, что открыл ch_mmdb_open_memory. MMDB_close сюда не подходит:
   он попытается сделать munmap на обычном буфере. */
void ch_mmdb_close_memory(MMDB_s *const mmdb);

#ifdef __cplusplus
}
#endif

#endif /* CH_GEOIP_MEMORY_H */
