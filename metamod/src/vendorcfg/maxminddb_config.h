/* Конфиг libmaxminddb, собираемый обычно autotools/CMake.
 *
 * Мы вендорим библиотеку и собираем её одним файлом (см. geoip_maxminddb.c),
 * поэтому генератор не запускается и заголовок задан явно. Оба значения
 * безопасны на всех платформах, куда собирается плагин: и GCC, и Clang, и MSVC
 * на x86-64 умеют 128-битный тип либо обходятся байтовым массивом.
 */
#ifndef MAXMINDDB_CONFIG_H
#define MAXMINDDB_CONFIG_H

#ifndef MMDB_UINT128_USING_MODE
#define MMDB_UINT128_USING_MODE 0
#endif

#ifndef MMDB_UINT128_IS_BYTE_ARRAY
#if defined(_MSC_VER) || !defined(__SIZEOF_INT128__)
#define MMDB_UINT128_IS_BYTE_ARRAY 1
#endif
#endif

#endif /* MAXMINDDB_CONFIG_H */
