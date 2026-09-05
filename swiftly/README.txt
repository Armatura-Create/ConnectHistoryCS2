Плагин ConnectHistory под SwiftlyS2.

SwiftlyS2 — не надстройка над Metamod:Source, а альтернативный лоадер: он подключается
строкой `Game csgo/addons/swiftlys2` в gameinfo.gi и Metamod не требует. Поэтому это
отдельная реализация, а не обёртка над версией для CounterStrikeSharp.

Что общего с остальными целями: только контракт базы данных (../docs/DATABASE.md).
Код продублирован сознательно — цели развиваются независимо.

Сборка:  ./build.sh [версия]
Тесты:   dotnet test ConnectHistory.sln

Тесты, которым нужна сборка SwiftlyS2, требуют x64-процесса: SwiftlyS2.CS2.dll собрана
только под x64, потому что выделенный сервер CS2 другой не бывает. На arm64-машине они
пропускаются с внятной причиной (см. tests/.../SwiftlyRuntime.cs), в CI выполняются все.
