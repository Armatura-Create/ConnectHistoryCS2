# Модуль для Flute CMS на данных ConnectHistory

Как построить в панели [Flute CMS](https://github.com/Flute-CMS/cms) раздел
«История и статистика игроков», читающий таблицы этого плагина.

Официальная документация по модулям: <https://docs.flute-cms.com/en/modules>.
Здесь — то, что специфично именно для наших данных.

## Что нужно знать про Flute

| | |
|---|---|
| Язык | PHP 8 |
| ORM | **Cycle ORM**, ActiveRecord-паттерн, сущности через атрибуты `#[Entity]` |
| Шаблоны | Blade |
| Модуль | каталог в `app/Modules/<Имя>/` с `module.json` и провайдером |
| Включение | Админ-панель → Модули → Активировать |

## Два способа подключиться к данным

**1. Таблицы плагина лежат в той же базе, что и Flute.** Самый простой путь:
описываете сущности Cycle ORM и работаете с ними как с родными таблицами панели.
Так сделано в примерах ниже.

**2. История в отдельной базе.** Тогда сущности Cycle не годятся: заводите в модуле
свой сервис поверх PDO с отдельными параметрами подключения и пишите SQL из
[DATABASE.md](DATABASE.md) напрямую. Готовые запросы там уже параметризованы —
их достаточно перенести в `prepare()`/`execute()`.

Второй вариант чаще правильный: у истории свой ретеншн, свой пользователь MySQL
и права только на чтение, и мешать её со схемой панели незачем.

## Структура модуля

```
app/Modules/ConnectHistory/
├── module.json
├── Providers/ConnectHistoryServiceProvider.php
├── Http/Controllers/HistoryController.php
├── database/Entities/ChSession.php
├── database/Entities/ChPlayer.php
├── Services/HistoryQuery.php          # для варианта с отдельной базой
└── Resources/
    ├── views/online.blade.php
    ├── views/player.blade.php
    ├── config/connecthistory.php
    └── lang/{ru,en}.php
```

### `module.json`

```json
{
    "name": "ConnectHistory",
    "version": "1.0.0",
    "description": "История подключений и статистика игроков",
    "providers": ["Flute\\Modules\\ConnectHistory\\Providers\\ConnectHistoryServiceProvider"]
}
```

### Провайдер

```php
<?php

namespace Flute\Modules\ConnectHistory\Providers;

use Flute\Core\Support\ModuleServiceProvider;

class ConnectHistoryServiceProvider extends ModuleServiceProvider
{
    public function boot(): void
    {
        // Подхватывает routes, views, lang, config и сущности модуля
        $this->bootstrapModule();
    }
}
```

## Сущности Cycle ORM

Колонки описаны в [DATABASE.md](DATABASE.md); ниже — минимум, которого хватает
для страниц «онлайн» и «профиль игрока».

```php
<?php

namespace Flute\Modules\ConnectHistory\database\Entities;

use Cycle\Annotated\Annotation\Column;
use Cycle\Annotated\Annotation\Entity;
use Cycle\Annotated\Annotation\Table\Index;
use Flute\Core\Database\Entities\ActiveRecord;

#[Entity(table: 'ch_sessions', role: 'ch_session')]
#[Index(columns: ['steamid64', 'started_at'])]
class ChSession extends ActiveRecord
{
    #[Column(type: 'primary')]
    public int $id;

    #[Column(type: 'string(32)', name: 'session_key')]
    public string $sessionKey;

    #[Column(type: 'bigInteger')]
    public string $steamid64;          // строка: 64 бита не влезают в int на 32-битных сборках

    #[Column(type: 'integer', name: 'server_id')]
    public int $serverId;

    #[Column(type: 'string(128)')]
    public string $nickname;

    #[Column(type: 'datetime', name: 'started_at')]
    public \DateTimeInterface $startedAt;

    #[Column(type: 'datetime', name: 'ended_at', nullable: true)]
    public ?\DateTimeInterface $endedAt = null;

    #[Column(type: 'integer', name: 'duration_seconds', nullable: true)]
    public ?int $durationSeconds = null;

    /** 0 open, 1 disconnect, 2 map change, 3 shutdown, 4 plugin unload, 5 stale */
    #[Column(type: 'integer', name: 'end_kind')]
    public int $endKind = 0;

    #[Column(type: 'string(64)', name: 'connect_map')]
    public string $connectMap = '';

    #[Column(type: 'string(2)', name: 'country_iso', nullable: true)]
    public ?string $countryIso = null;

    #[Column(type: 'integer', nullable: true)]
    public ?int $kills = null;

    #[Column(type: 'integer', nullable: true)]
    public ?int $deaths = null;

    public function isOnline(): bool
    {
        return $this->endedAt === null && $this->endKind === 0;
    }
}
```

```php
#[Entity(table: 'ch_players', role: 'ch_player')]
class ChPlayer extends ActiveRecord
{
    #[Column(type: 'bigInteger', primary: true)]
    public string $steamid64;

    #[Column(type: 'datetime', name: 'first_seen')]
    public \DateTimeInterface $firstSeen;

    #[Column(type: 'datetime', name: 'last_seen')]
    public \DateTimeInterface $lastSeen;

    #[Column(type: 'integer', name: 'sessions_count')]
    public int $sessionsCount = 0;

    #[Column(type: 'bigInteger', name: 'total_seconds')]
    public int $totalSeconds = 0;

    #[Column(type: 'string(128)', name: 'last_nickname')]
    public string $lastNickname = '';

    public function hours(): float
    {
        return round($this->totalSeconds / 3600, 1);
    }
}
```

> **Важно:** плагин создаёт и мигрирует свои таблицы сам. Не давайте модулю
> Flute генерировать по этим сущностям миграции — сущности здесь только для чтения.

## Контроллер

```php
<?php

namespace Flute\Modules\ConnectHistory\Http\Controllers;

use Flute\Core\Support\FluteController;
use Flute\Modules\ConnectHistory\database\Entities\ChPlayer;
use Flute\Modules\ConnectHistory\database\Entities\ChSession;
use Symfony\Component\HttpFoundation\Response;

class HistoryController extends FluteController
{
    /** Кто сейчас на сервере. Открытая сессия — это и есть «онлайн». */
    public function online(int $serverId = 1): Response
    {
        $players = ChSession::query()
            ->where('server_id', $serverId)
            ->where('ended_at', null)
            ->where('end_kind', 0)
            ->orderBy('started_at', 'ASC')
            ->fetchAll();

        return view('ConnectHistory::online', [
            'players' => $players,
            'serverId' => $serverId,
        ]);
    }

    /** Топ по наигранному времени. */
    public function top(): Response
    {
        return view('ConnectHistory::top', [
            'players' => ChPlayer::query()
                ->orderBy('total_seconds', 'DESC')
                ->limit(50)
                ->fetchAll(),
        ]);
    }

    /** Профиль: агрегаты + последние сессии. */
    public function player(string $steamid64): Response
    {
        $player = ChPlayer::findByPK($steamid64);
        if ($player === null) {
            return $this->error(__('connecthistory.not_found'), 404);
        }

        return view('ConnectHistory::player', [
            'player' => $player,
            'sessions' => ChSession::query()
                ->where('steamid64', $steamid64)
                ->where('ended_at', '!=', null)
                ->orderBy('started_at', 'DESC')
                ->limit(20)
                ->fetchAll(),
        ]);
    }
}
```

Маршруты в Flute объявляются атрибутами прямо на методах контроллера —
отдельного файла роутов нет; актуальный синтаксис смотрите в
[документации по контроллерам](https://docs.flute-cms.com/en/modules).

## Blade-шаблон

```blade
{{-- Resources/views/online.blade.php --}}
<div class="card">
    <div class="card-header">
        {{ __('connecthistory.online') }}: {{ count($players) }}
    </div>
    <table class="table">
        <thead>
            <tr>
                <th>{{ __('connecthistory.player') }}</th>
                <th>{{ __('connecthistory.since') }}</th>
                <th>{{ __('connecthistory.map') }}</th>
            </tr>
        </thead>
        <tbody>
            @forelse ($players as $session)
                <tr>
                    <td>
                        <a href="/history/player/{{ $session->steamid64 }}">
                            {{ $session->nickname }}
                        </a>
                    </td>
                    <td>{{ $session->startedAt->format('H:i') }}</td>
                    <td>{{ $session->connectMap }}</td>
                </tr>
            @empty
                <tr><td colspan="3">{{ __('connecthistory.empty') }}</td></tr>
            @endforelse
        </tbody>
    </table>
</div>
```

## Сервис для отдельной базы

Когда история живёт не в базе панели — вместо сущностей один сервис на PDO:

```php
<?php

namespace Flute\Modules\ConnectHistory\Services;

use PDO;

class HistoryQuery
{
    private PDO $pdo;

    public function __construct(array $config)
    {
        $dsn = sprintf(
            'mysql:host=%s;port=%d;dbname=%s;charset=utf8mb4',
            $config['host'], $config['port'], $config['database']
        );

        // Пользователь только на чтение: панели запись в историю не нужна
        $this->pdo = new PDO($dsn, $config['user'], $config['password'], [
            PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES => false,
        ]);
    }

    public function online(int $serverId): array
    {
        $stmt = $this->pdo->prepare(
            'SELECT nickname, steamid64, started_at, connect_map,
                    TIMESTAMPDIFF(SECOND, started_at, UTC_TIMESTAMP()) AS online_seconds
             FROM ch_sessions
             WHERE server_id = :server AND ended_at IS NULL AND end_kind = 0
             ORDER BY started_at'
        );
        $stmt->execute(['server' => $serverId]);

        return $stmt->fetchAll();
    }

    public function playerCard(string $steamid64): ?array
    {
        $stmt = $this->pdo->prepare(
            'SELECT last_nickname, first_seen, last_seen, sessions_count, total_seconds
             FROM ch_players WHERE steamid64 = :steam'
        );
        $stmt->execute(['steam' => $steamid64]);

        return $stmt->fetch() ?: null;
    }
}
```

Регистрация в провайдере:

```php
$this->app->singleton(HistoryQuery::class, fn () => new HistoryQuery(config('connecthistory.db')));
```

## Что стоит учесть

- **Время в базе — UTC.** Переводите в таймзону панели при выводе, иначе «последний заход» будет врать на несколько часов.
- **`steamid64` — строка.** 64-битное число в PHP на 32-битных сборках теряет точность.
- **Онлайн-виджет кешируйте на 15–30 секунд.** Запрос дешёвый, но страница может открываться сотнями людей одновременно.
- **`end_kind = 5` — не мусор, а факт падения сервера.** Не прячьте такие сессии: по ним строится карта аварий (см. запрос в [DATABASE.md](DATABASE.md)).
- **Не показывайте `player_ip` обычным пользователям.** Ограничьте колонку правом уровня админа, а лучше не выбирайте её в публичных запросах вовсе.
- **Аватары и имена Steam** берите через Steam Web API по `steamid64` — плагин их не хранит.

## Ссылки

- [Flute-CMS/cms](https://github.com/Flute-CMS/cms) — исходники панели
- [docs.flute-cms.com/en/modules](https://docs.flute-cms.com/en/modules) — документация по модулям
- [Flute-CMS/Monitoring](https://github.com/Flute-CMS/Monitoring) — готовый модуль как образец структуры
