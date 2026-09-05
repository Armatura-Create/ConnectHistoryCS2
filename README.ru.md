# ConnectHistory (CS2)

[English](README.md) | **Русский**

[![CI](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci.yml/badge.svg)](https://github.com/Armatura-Create/ConnectHistoryCS2/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Armatura-Create/ConnectHistoryCS2?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Armatura-Create/ConnectHistoryCS2/total?logo=github&color=success)](https://github.com/Armatura-Create/ConnectHistoryCS2/releases)
[![.NET 10](https://img.shields.io/badge/.NET-10.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![CounterStrikeSharp](https://img.shields.io/badge/CounterStrikeSharp-%E2%89%A5%201.0.369-1f6feb?logo=steam)](https://github.com/roflmuffin/CounterStrikeSharp)
[![MySQL](https://img.shields.io/badge/MySQL-5.7%20%7C%208.0%2B-4479A1?logo=mysql&logoColor=white)](https://www.mysql.com/)
[![Platforms](https://img.shields.io/badge/Platforms-Linux%20%7C%20Windows-2ea44f)](#установка)
[![GeoLite2](https://img.shields.io/badge/GeoLite2-в%20архиве%20%2F%20авто--обновление-009688)](#сборка)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue)](LICENSE)

История подключений и аналитика игроков для CounterStrikeSharp / CS2. Каждая сессия
пишется в MySQL вместе с картой, гео, игровыми итогами и качеством связи — готовые
данные для веб-панели.

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

> Нужны **CounterStrikeSharp v1.0.369+** и **MySQL 5.7 / 8.0+ (или MariaDB 10.4+)**.
> v1.0.369 — первая версия на .NET 10, а плагин собран под `net10.0`.

1. Скачайте `ConnectHistory.zip` из [последнего релиза](https://github.com/Armatura-Create/ConnectHistoryCS2/releases/latest)
   и распакуйте в корень игрового сервера:

```
addons/counterstrikesharp/plugins/ConnectHistory/
├── ConnectHistory.dll
├── MySqlConnector.dll
├── MaxMind.GeoIP2.dll
├── MaxMind.Db.dll
├── GeoLite2-Country.mmdb
└── GeoLite2-City.mmdb
```

2. Запустите сервер — плагин сам создаст конфиги и таблицы в базе.
3. Заполните секцию `Database` в `Settings.json` и выполните `css_ch_reload`.

В `Settings.json` лежит пароль от базы, поэтому плагин держит файл с правами `600`
и никогда не кладёт его в релизный архив.

## Конфигурация

`csgo/addons/counterstrikesharp/configs/plugins/ConnectHistory/`

| Файл | Что в нём |
|---|---|
| `Settings.json` | база, что собирать, хранение, команды |
| `Messages.json` | тексты игроцких команд по языкам |
| `*.schema.json` | JSON Schema, перезаписывается при каждой загрузке |
| `README.txt` | короткая справка, перезаписывается при каждой загрузке |

Основное:

```jsonc
{
  "ServerId": 1,              // у разных серверов обязан отличаться
  "DisplayTimeZone": "UTC",   // что видят ИГРОКИ: "Europe/Moscow", "UTC" или "Local".
                              // В базе всегда UTC — на неё эта настройка не влияет.
  "Server": {
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
    "OnlineSnapshots": true
  }
}
```

## Команды

| Команда | Права | Действие |
|---|---|---|
| `css_ch_status` | `@css/root` | связь с базой, размер очереди, последняя ошибка |
| `css_ch_reload` | `@css/root` | перечитать конфигурацию |
| `css_playtime` | любой игрок | своё наигранное время и число заходов |
| `css_lastseen` | любой игрок | свои последние сессии |

## База данных

Шесть таблиц, все создаёт сам плагин: `ch_sessions`, `ch_players`, `ch_nicknames`,
`ch_servers`, `ch_online_snapshots`, `ch_schema_version`.

Полная схема и готовые запросы: [docs/DATABASE.md](docs/DATABASE.md).

Готовый раздел для панели Flute CMS —
[ConnectHistory-Flute](https://github.com/Armatura-Create/ConnectHistory-Flute):
онлайн, статистика, фильтры и графики поверх этих таблиц.
Как связать — [docs/FLUTE_MODULE.md](docs/FLUTE_MODULE.md).

Заведите плагину отдельного пользователя MySQL — `DROP` и `DELETE` ему не нужны никогда:

```sql
GRANT SELECT, INSERT, UPDATE, CREATE, INDEX, ALTER ON connect_history.* TO 'ch_plugin'@'%';
```

## Сборка

```bash
export PATH="$HOME/.dotnet:$PATH"
./build.sh            # restore, сборка, тесты, архив в bin/Release/net10.0/ConnectHistory.zip
```

Или руками:

```bash
dotnet build -c Release      # заодно собирает zip
dotnet test                  # xUnit
```

Интеграционные тесты с MySQL пропускаются, пока не задан `CH_TEST_MYSQL`:

```bash
docker run --rm -d -p 3399:3306 -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=ch mysql:8
export CH_TEST_MYSQL="server=127.0.0.1;port=3399;user=root;password=test;database=ch"
dotnet test
```

## Благодарности

Данные GeoLite2 — [MaxMind](https://www.maxmind.com), распространяются по
[GeoLite2 End User License Agreement](https://www.maxmind.com/en/geolite2/eula).

## Лицензия

[GPL-3.0-or-later](LICENSE).
