using System;
using SwiftlyS2.Shared.Events;
using SwiftlyS2.Shared.GameEventDefinitions;
using SwiftlyS2.Shared.GameEvents;
using SwiftlyS2.Shared.Misc;

namespace ConnectHistory;

/// Единственная точка регистрации обработчиков.
///
/// Каждый обработчик обёрнут в SafeEvent: исключение в нашем коде не должно всплывать
/// во фреймворк и мешать другим плагинам на сервере.
///
/// Подписки на Core.Event.* снимаются в Unload по СОХРАНЁННОМУ делегату: `-=` с новой
/// лямбдой ничего не отпишет, и после hot reload обработчик отработает дважды.
public sealed partial class ConnectHistory
{
    private Guid? _hookConnectFull;
    private Guid? _hookDisconnect;
    private Guid? _hookRoundEnd;
    private Guid? _hookPlayerTeam;

    private EventDelegates.OnMapLoad? _onMapLoad;

    private void RegisterEvents()
    {
        _hookConnectFull = Core.GameEvent.HookPost<EventPlayerConnectFull>(SafeEvent<EventPlayerConnectFull>(OnPlayerConnectFull));
        _hookDisconnect = Core.GameEvent.HookPost<EventPlayerDisconnect>(SafeEvent<EventPlayerDisconnect>(OnPlayerDisconnect));
        _hookRoundEnd = Core.GameEvent.HookPost<EventRoundEnd>(SafeEvent<EventRoundEnd>(OnRoundEnd));
        _hookPlayerTeam = Core.GameEvent.HookPost<EventPlayerTeam>(SafeEvent<EventPlayerTeam>(OnPlayerTeam));

        // Смена карты — граница сессии: так в данных остаётся, СКОЛЬКО игрок провёл
        // на конкретной карте, а не размазанное по нескольким картам время.
        _onMapLoad = _ => SafeRun("смена карты", OnMapChanged);
        Core.Event.OnMapLoad += _onMapLoad;
    }

    private void UnregisterEvents()
    {
        foreach (var hook in new[] { _hookConnectFull, _hookDisconnect, _hookRoundEnd, _hookPlayerTeam })
        {
            if (hook.HasValue) Core.GameEvent.Unhook(hook.Value);
        }

        _hookConnectFull = null;
        _hookDisconnect = null;
        _hookRoundEnd = null;
        _hookPlayerTeam = null;

        if (_onMapLoad != null)
        {
            Core.Event.OnMapLoad -= _onMapLoad;
            _onMapLoad = null;
        }
    }

    private IGameEventService.GameEventHandler<T> SafeEvent<T>(IGameEventService.GameEventHandler<T> handler)
        where T : IGameEvent<T>
        => ev =>
        {
            try
            {
                return handler(ev);
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
