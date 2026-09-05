#!/usr/bin/env bash
# Локальная сборка: restore -> тесты -> Release -> архив.
# Гита в проекте нет, поэтому CI заменяет этот скрипт.
set -euo pipefail

cd "$(dirname "$0")"

# SDK стоит в ~/.dotnet и в PATH по умолчанию не попадает
export PATH="$HOME/.dotnet:$PATH"

VERSION="${1:-}"
VERSION_ARG=()
if [[ -n "$VERSION" ]]; then
  VERSION_ARG=("-p:Version=${VERSION#v}")
  echo "==> Версия сборки: ${VERSION#v}"
fi

echo "==> Restore"
dotnet restore ConnectHistory.sln

echo "==> Тесты"
# Интеграционные тесты MySQL пропускаются, если не задан CH_TEST_MYSQL
dotnet test ConnectHistory.sln -c Release --nologo

echo "==> Release + упаковка"
# ${arr[@]+...} — иначе bash 3.2 (штатный на macOS) падает на пустом массиве при set -u
dotnet build ConnectHistory.csproj -c Release --nologo ${VERSION_ARG[@]+"${VERSION_ARG[@]}"}

# Имя архива несёт версию, а она известна только MSBuild (из аргумента или из .csproj),
# поэтому ищем по маске. Прошлые версии PackageRelease удаляет сам — файл ровно один.
ZIP="$(ls -1 bin/Release/net10.0/ConnectHistory_cssharp_*.zip 2>/dev/null | head -n 1)"
if [[ -z "$ZIP" || ! -f "$ZIP" ]]; then
  echo "Архив не собрался: bin/Release/net10.0/ConnectHistory_cssharp_*.zip" >&2
  exit 1
fi

echo
echo "Готово: $(cd "$(dirname "$ZIP")" && pwd)/$(basename "$ZIP")"
unzip -l "$ZIP"
