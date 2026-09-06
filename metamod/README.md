# ConnectHistory — цель Metamod:Source

Нативный плагин на C++. **Не** обёртка над версиями для CounterStrikeSharp или
SwiftlyS2: Metamod не хостит .NET, поэтому это отдельная реализация. Общий
с остальными целями — только контракт базы данных (`../docs/DATABASE.md`).

Минимум — **Metamod:Source 2.0.0-git1322**. Это не круглое число и не запас
на всякий случай: в этой сборке появилась поддержка ConVar и ConCommand для
Source 2, а без неё `META_CONVAR_REGISTER` не существует и команды плагина
просто не попадают в консоль сервера.

## Что можно проверить локально

Ядро (`src/core`) сознательно не знает ни про hl2sdk, ни про MySQL:

```bash
make -f Makefile.tests -j8 && ./build-tests/ch_tests
```

Сто с лишним тестов проходят за секунду на любой машине с компилятором C++17 —
включая открытие настоящих баз GeoLite2 из `../GeoIP/`. Здесь живут все
инварианты: идемпотентность закрытия сессии, спул, разбор адреса сервера,
цвета чата, устойчивость конфига к правкам руками.

**Зелёная локальная сборка не равна зелёной сборке в CI.** На macOS это Apple clang
с libc++, в CI — g++ с libstdc++, и различия не косметические:

* libc++ подтягивает `<cstdint>` транзитивно, libstdc++ — нет. Заголовок, забывший
  этот `#include`, соберётся локально и упадёт в CI. Все заголовки обязаны включать
  то, что используют, — на это уже наступали.
* clang и gcc по-разному строги к `__int128` под `-Wpedantic` (у нас `-Werror`).
  Вендорный `maxminddb.h` объявляет его, поэтому включение обёрнуто в
  `#pragma GCC diagnostic` — точечно, не на весь проект.

* Игровая сборка идёт с `-fno-exceptions` (так собран весь hl2sdk) и `-std=c++20`,
  а тесты — с исключениями и `c++17`. В режиме без исключений nlohmann/json на
  ошибке зовёт `std::abort()`, то есть кривой `Settings.json` убивал бы сервер
  вместо отката на значения по умолчанию. Обе дыры закрывает
  `make -f Makefile.tests noexcept-check` — он компилирует ядро ровно так, как это
  делает AMBuild.

Перед пушем стоит прогнать обе цели:

```bash
make -f Makefile.tests -j8 && ./build-tests/ch_tests   # поведение
make -f Makefile.tests noexcept-check                  # совместимость с игровой сборкой
```

Различия компиляторов этим не покрываются — для них самое дешёвое:
`docker run --rm -v "$PWD":/w -w /w gcc:13 make -f Makefile.tests`.

## Что собирается только в CI

Слой движка (`src/mm`), клиент MySQL (`src/db`) и упаковка требуют hl2sdk,
Metamod:Source и статического клиента MariaDB. Собрать это на машине
разработчика под macOS нельзя вовсе, поэтому цепочка живёт в
`.github/workflows/ci-metamod.yml`: контейнер Steam Runtime 3 для Linux
и windows-latest для Windows.

Локально (Linux) то же самое:

```bash
git clone --recurse-submodules https://github.com/alliedmodders/hl2sdk -b cs2 sdk
git clone https://github.com/alliedmodders/hl2sdk-manifests
git clone --recurse-submodules https://github.com/alliedmodders/metamod-source ../../mmsource-2.0
cmake -S vendor/mariadb-connector-c -B build-deps/mariadb -DCMAKE_BUILD_TYPE=Release -DWITH_UNIT_TESTS=OFF
cmake --build build-deps/mariadb --target mariadbclient
mkdir -p build-deps/lib && find build-deps/mariadb -name 'libmariadbclient.a' -exec cp {} build-deps/lib/ \;
mkdir build && cd build && python ../configure.py --enable-optimize --sdks cs2 && ambuild
```

## Осознанные ограничения этой цели

Плагин **не использует ни одной сигнатуры, ни одного смещения и ни одного
детура**. Всё нужное доступно через хуки Metamod, игровые события и интерфейсы
движка. Цена — три расхождения с C#-целями, все зафиксированы
в `../docs/DATABASE.md`:

| Что | Почему |
|---|---|
| `score` всегда `NULL` | В CS2 нет игрового события со счётом, а поле контроллера требует указателя на `CGameEntitySystem` по захардкоженному смещению — ломающемуся в любое обновление игры |
| `ping_avg/min/max/samples` всегда `NULL` | Ровно то же: пинг живёт только в поле контроллера |
| `ch_playtime` / `ch_lastseen` — консольные команды с аргументом SteamID | Отправка сообщения конкретному игроку идёт через UserMessage с протобуфами SDK; «почти работающая» команда, печатающая ответ не туда, хуже честной консольной |

Убийства, смерти, помощь, урон, хедшоты, MVP и раунды при этом собираются
полностью — из игровых событий, а не из полей контроллера.

`DisplayTimeZone` понимает `UTC`, `Local` и смещение вида `+03:00`; имена IANA
(`Europe/Moscow`) не поддержаны — для них в C++ нужна либо tzdata через C++20
`<chrono>` (её нет в MSVC-сборке под Windows), либо правка глобальной `TZ`
процесса, общей на весь игровой сервер. На то, что пишется в базу, настройка
не влияет: там всегда UTC.

## Вендоры

| Библиотека | Зачем | Лицензия |
|---|---|---|
| `mariadb-connector-c` | MySQL | LGPL-2.1 — совместима с GPL-3.0-or-later |
| `libmaxminddb` | GeoLite2 | Apache-2.0 |
| `nlohmann/json` | конфиги | MIT |
| `doctest` | тесты | MIT |

`libmysqlclient` не берём сознательно: его GPL-2.0-with-FOSS-exception
конфликтует с GPL-3.

`AMBuildScript` и `configure.py` взяты из [CS2Fixes](https://github.com/Source2ZE/CS2Fixes)
(GPL-3.0) и адаптированы: своя версия означала бы повторять по памяти три сотни
строк логики определения hl2sdk и разъезжаться с ней при каждом обновлении SDK.
