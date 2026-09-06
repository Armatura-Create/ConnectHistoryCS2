# ConnectHistory (CS2)

[English](README.md) | **Русский**

[![CI CSSharp](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-cssharp.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-cssharp.yml)
[![CI SwiftlyS2](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-swiftly.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-swiftly.yml)
[![CI Metamod](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-metamod.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci-metamod.yml)
[![Release](https://img.shields.io/github/v/release/Armatura-Create/ConnectHistoryCS2?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Armatura-Create/ConnectHistoryCS2/total?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases)
[![.NET 10](https://img.shields.io/badge/.NET-10.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![CounterStrikeSharp](https://img.shields.io/badge/CounterStrikeSharp-%E2%89%A5%201.0.369-1f6feb?logo=steam)](https://github.com/roflmuffin/CounterStrikeSharp)
[![SwiftlyS2](https://img.shields.io/badge/SwiftlyS2-%E2%89%A5%201.4.9-8957e5)](https://github.com/swiftly-solution/swiftlys2)
[![Metamod:Source](https://img.shields.io/badge/Metamod%3ASource-%E2%89%A5%202.0.0--git1322-f0883e)](https://www.sourcemm.net/downloads.php?branch=master)
[![MySQL](https://img.shields.io/badge/MySQL-5.7%20%7C%208.0%2B-4479A1?logo=mysql&logoColor=white)](https://www.mysql.com/)
[![Platforms](https://img.shields.io/badge/Platforms-Linux%20%7C%20Windows-2ea44f)](#установка)
[![GeoLite2](https://img.shields.io/badge/GeoLite2-в%20архиве%20%2F%20авто--обновление-009688)](#сборка)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue)](LICENSE)

История подключений и аналитика игроков для CS2. Каждая сессия пишется в MySQL
вместе с картой, гео, игровыми итогами и качеством связи — готовые данные
для веб-панели.

**Три реализации под три платформы, одна схема базы.** Плагин существует отдельно
под CounterStrikeSharp, SwiftlyS2 и нативным Metamod:Source. Код у них не общий —
общая схема, и все три пишут в одни таблицы. Можно ставить разные плагины
на разные серверы и строить один отчёт.

| Платформа | Требования | Архив | Куда ставится |
|---|---|---|---|
| **CounterStrikeSharp** | CSSharp ≥ 1.0.369 (значит, и Metamod:Source) | `ConnectHistory_cssharp_<версия>.zip` | `addons/counterstrikesharp/plugins/ConnectHistory/` |
| **SwiftlyS2** | SwiftlyS2 ≥ 1.4.9, Metamod **не нужен** | `ConnectHistory_swiftly_<версия>.zip` | `addons/swiftlys2/plugins/ConnectHistory/` |
| **Metamod:Source** | Metamod:Source ≥ 2.0.0-git1322, больше ничего | `ConnectHistory_metamod_{linux,windows}_<версия>.zip` | `addons/ConnectHistory/` |

Нативная цель пишет историю подключений полностью, но не собирает игровые
итоги — чего именно нет и почему, написано в
[«Расхождения между целями»](docs/DATABASE.md#differences-between-targets).

## Возможности

- **Сессия открывается на входе и закрывается на выходе.** Упавший сервер оставляет видимый след, а не пустоту.
- **«Кто сейчас онлайн» — обычный `SELECT`**, без RCON и A2S-запросов.
- **Полные данные сессии**: SteamID64, ник, карта, причина отключения, страна и город, IP (или его хеш с солью), убийства/смерти/помощи/урон/MVP/счёт, раунды, команда, средний/мин/макс пинг.
- **Агрегаты** — наигранное время, число заходов, первый и последний визит, история ников.
- **Снимки онлайна** для графика посещаемости.
- **Переживает недоступность базы** — записи копятся на диске и досылаются автоматически.
- **Не блокирует игровой поток** — один фоновый писатель и маленький пул соединений.
- **JSON-конфиг со схемой** — редактор подсказывает поля и подсвечивает опечатки.

## Установка

> Нужен **MySQL 5.7 / 8.0+ (или MariaDB 10.4+)**. Остальное зависит от платформы —
> см. таблицу выше.

1. Скачайте архив своей платформы из
   [последнего релиза](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
   и распакуйте в корень игрового сервера. Структура каталогов уже внутри,
   вместе с базами GeoLite2.

<details>
<summary>CounterStrikeSharp</summary>

```
addons/counterstrikesharp/plugins/ConnectHistory/
├── ConnectHistory.dll
├── MySqlConnector.dll
├── MaxMind.GeoIP2.dll
├── MaxMind.Db.dll
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```
</details>

<details>
<summary>SwiftlyS2</summary>

```
addons/swiftlys2/plugins/ConnectHistory/
├── ConnectHistory.dll
├── MySqlConnector.dll
├── MaxMind.GeoIP2.dll
├── MaxMind.Db.dll
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```
</details>

<details>
<summary>Metamod:Source</summary>

По архиву на ОС — берите `..._linux_...` или `..._windows_...`.

```
addons/metamod/ConnectHistory.vdf
addons/ConnectHistory/
├── bin/linuxsteamrt64/ConnectHistory.so   (или bin/win64/ConnectHistory.dll)
├── configs/
│   ├── Settings.json
│   ├── Messages.json
│   ├── Settings.schema.json
│   ├── Messages.schema.json
│   └── README.txt
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```

Конфиги по умолчанию уже лежат в архиве — плагин работает и там, где каталог
сервера доступен только на чтение. Раскладку менять не нужно: конфиги плагин
ищет в `csgo/addons/ConnectHistory/configs/` независимо от того, где лежит сам
бинарник, и пишет в консоль, если добраться туда не смог.

**При обновлении не распаковывайте `configs/` поверх существующей установки** —
это затрёт ваш `Settings.json` вместе с паролем от базы.
</details>

2. Запустите сервер — плагин создаст недостающие конфиги (вместе с родительскими
   каталогами) и таблицы в базе. Если записать их не удалось, он скажет об этом
   в консоли и назовёт точный путь, а не будет молча работать на дефолтах.
3. Заполните секцию `Database` в `Settings.json` и перезагрузите конфигурацию
   (`css_ch_reload`, `sw_ch_reload` или `ch_reload` — по платформе).

В `Settings.json` лежит пароль от базы, поэтому плагин держит файл с правами `600`.
В архивах едет только **шаблон** с пустым паролем — релиз с заполненным не соберётся.

## Конфигурация

| Платформа | Каталог конфигов |
|---|---|
| CounterStrikeSharp | `csgo/addons/counterstrikesharp/configs/plugins/ConnectHistory/` |
| SwiftlyS2 | `csgo/addons/swiftlys2/configs/plugins/ConnectHistory/` |
| Metamod:Source | `csgo/addons/ConnectHistory/configs/` (фиксированный, не рядом с бинарником) |

Формат файлов одинаковый: `Settings.json` и `Messages.json` переносятся между
платформами без правок.

| Файл | Что в нём |
|---|---|
| `Settings.json` | база, что собирать, хранение, команды |
| `Messages.json` | тексты игроцких команд по языкам |
| `*.schema.json` | JSON Schema, перезаписывается при каждой загрузке |
| `README.txt` | короткая справка, перезаписывается при каждой загрузке |

Основное:

```jsonc
{
  "DisplayTimeZone": "UTC",   // что видят ИГРОКИ: "Europe/Moscow", "UTC" или "Local".
                              // В базе всегда UTC — на неё эта настройка не влияет.
  "Server": {
    "Id": 1,                  // у разных серверов обязан отличаться. До 3.0.1 лежал
                              // в корне как "ServerId"; старое написание ещё читается.
    "PublicAddress": ""       // "ip:port" или "host:port". Пусто — определить самому,
                              // но процесс сервера своего публичного адреса не знает:
                              // ConVar ip отдаёт адрес привязки сокета, обычно 0.0.0.0.
  },
  "Database": {
    "Host": "127.0.0.1",
    "SslMode": "Required",    // для базы вне localhost — обязательно
    "TablePrefix": "ch_"
  },
  "Collect": {
    "GeoIp": true,
    "PlayerIp": true,         // персональные данные — выключается независимо
    "IpHash": true,
    "IpHashSalt": "",         // пустая соль отключает хеширование: хеш без соли обратим
    "MatchStats": true,
    "Ping": true,
    "CountSpectatorTime": true,  // false — не считать наигранным время
                                // в наблюдателях и без команды
    "OnlineSnapshots": true
  }
}
```

## Команды

| Действие | CounterStrikeSharp | SwiftlyS2 | Metamod |
|---|---|---|---|
| связь с базой, очередь, последняя ошибка | `css_ch_status` (`@css/root`) | `sw_ch_status` | `ch_status` (консоль сервера) |
| перечитать конфигурацию | `css_ch_reload` (`@css/root`) | `sw_ch_reload` | `ch_reload` (консоль сервера) |
| наигранное время | `css_playtime` (игрок) | `sw_playtime` (игрок) | `ch_playtime <steamid64>` (консоль) |
| последние заходы | `css_lastseen` (игрок) | `sw_lastseen` (игрок) | `ch_lastseen <steamid64>` (консоль) |

В нативной цели команды игрока сделаны консольными: отправка сообщения конкретному
игроку идёт через UserMessage с протобуфами SDK, и «почти работающая» команда,
печатающая ответ не туда, хуже честной консольной.

## База данных

Шесть таблиц, все создаёт сам плагин: `ch_sessions`, `ch_players`, `ch_nicknames`,
`ch_servers`, `ch_online_snapshots`, `ch_schema_version`.

Полная схема и готовые запросы: [docs/DATABASE.md](docs/DATABASE.md).

Готовый раздел для панели Flute CMS:
**[ConnectHistory-Flute](https://github.com/Armatura-Create/ConnectHistory-Flute)** —
онлайн, статистика, фильтры и графики поверх этих таблиц. Установка и настройка
описаны в том репозитории.

Заведите плагину отдельного пользователя MySQL — `DROP` и `DELETE` ему не нужны никогда:

```sql
GRANT SELECT, INSERT, UPDATE, CREATE, INDEX, ALTER ON connect_history.* TO 'ch_plugin'@'%';
```

## Сборка

Каждая цель собирается отдельно.

```bash
export PATH="$HOME/.dotnet:$PATH"

cd cssharp && ./build.sh      # тесты + архив в bin/Release/net10.0/
cd swiftly && ./build.sh      # то же самое
```

Интеграционные тесты с MySQL пропускаются, пока не задан `CH_TEST_MYSQL`:

```bash
docker run --rm -d -p 3399:3306 -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=ch mysql:8
export CH_TEST_MYSQL="server=127.0.0.1;port=3399;user=root;password=test;database=ch"
dotnet test cssharp/ConnectHistory.sln
```

Нативная цель разделена надвое. Ядро не знает ни про hl2sdk, ни про MySQL
и проверяется на любой машине за секунду:

```bash
cd metamod && make -f Makefile.tests -j8 && ./build-tests/ch_tests
```

Сборка самого плагина требует hl2sdk, Metamod:Source и статического клиента
MariaDB — она живёт в CI. Подробности: [metamod/README.md](metamod/README.md).

## Благодарности

Данные GeoLite2 — [MaxMind](https://www.maxmind.com), распространяются по
[GeoLite2 End User License Agreement](https://www.maxmind.com/en/geolite2/eula).

## Лицензия

[GPL-3.0-or-later](LICENSE).
