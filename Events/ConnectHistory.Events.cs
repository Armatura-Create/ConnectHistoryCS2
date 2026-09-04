using System;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Events;

namespace ConnectHistory;

/// Единственная точка регистрации обработчиков.
///
/// Каждый обработчик обёрнут в SafeEvent: исключение в нашем коде не должно всплывать
/// во фреймворк и мешать другим плагинам на сервере.
public sealed partial class ConnectHistory
{
    private void RegisterEvents()
    {
        RegisterEventHandler<EventPlayerConnectFull>(SafeEvent<EventPlayerConnectFull>(OnPlayerConnectFull));
        RegisterEventHandler<EventPlayerDisconnect>(SafeEvent<EventPlayerDisconnect>(OnPlayerDisconnect));
        RegisterEventHandler<EventRoundEnd>(SafeEvent<EventRoundEnd>(OnRoundEnd));
        RegisterEventHandler<EventPlayerTeam>(SafeEvent<EventPlayerTeam>(OnPlayerTeam));

        // Смена карты — граница сессии: так в данных остаётся, СКОЛЬКО игрок провёл
        // на конкретной карте, а не размазанное по нескольким картам время.
        RegisterListener<Listeners.OnMapStart>(_ => SafeRun("смена карты", OnMapChanged));
    }

    private BasePlugin.GameEventHandler<T> SafeEvent<T>(BasePlugin.GameEventHandler<T> handler) where T : GameEvent
        => (ev, info) =>
        {
            try
            {
                return handler(ev, info);
            }
            catch (Exception ex)
            {
                _logger.Error($"[Event] Необработанная ошибка в {typeof(T).Name}", ex);
                return HookResult.Continue;
            }
        };

    private void OnMapChanged()
    {
        // Сначала закрываем то, что было: игроки, оставшиеся на сервере, откроют
        // новые сессии своим player_connect_full уже на новой карте.
        CloseAllSessions(SessionEndKind.MapChange);
        RegisterServer();
    }
}
