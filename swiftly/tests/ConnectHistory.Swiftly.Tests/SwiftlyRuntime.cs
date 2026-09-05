using System.Runtime.InteropServices;

namespace ConnectHistory.Tests;

/// Можно ли в этом процессе загрузить сборку SwiftlyS2.
///
/// SwiftlyS2.CS2.dll собрана под x64 — иначе и быть не может, выделенный сервер CS2
/// существует только для x64. На arm64-машине разработчика загрузка падает с
/// «assembly architecture is not compatible», и тесты, которым нужна эта сборка,
/// честнее пропустить с внятной причиной, чем показать пятнадцать одинаковых
/// FileNotFoundException.
///
/// Условие — АРХИТЕКТУРА, а не «получилось ли загрузить»: в CI (ubuntu-latest, x64)
/// оно никогда не выполняется, и настоящая поломка загрузки не спрячется за пропуском.
///
/// ВАЖНО про размещение проверок. Skip.IfNot в начале тела теста НЕ спасает: JIT
/// разрешает все типы, упомянутые в методе, ещё до выполнения первой строки, и метод
/// падает на входе. Поэтому обращения к типам SwiftlyS2 вынесены во вложенные классы
/// Bound — их загрузка откладывается до фактического вызова, который до пропуска
/// не доходит.
internal static class SwiftlyRuntime
{
    public static bool Available => RuntimeInformation.ProcessArchitecture == Architecture.X64;

    public const string SkipReason =
        "Сборка SwiftlyS2.CS2 собрана под x64 и в этом процессе не загружается. " +
        "Тест выполняется в CI (x64).";
}
