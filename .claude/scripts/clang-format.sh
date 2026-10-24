#!/usr/bin/env bash
# clang-format по C++ файлам, изменённым относительно HEAD, или по перечисленным.
#
#   .claude/scripts/clang-format.sh [--check] [файл...]
#
#   --check   только проверить, файлы не трогать
#
# Код возврата: 0 — формат чист (или приведён), 1 — с --check нашлись неотформатированные
# файлы, 2 — проверить не получилось.
#
# Вывод — одна строка итога и имена файлов, которые форматирование меняет: diff
# не печатается, потому что поправить файл дешевле, чем его прочитать.
set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$root" || exit 2

check=false
args=()
for arg in "$@"; do
    case "$arg" in
    --check) check=true ;;
    *) args+=("$arg") ;;
    esac
done

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format: не найден в PATH"
    exit 2
fi

if [ ${#args[@]} -gt 0 ]; then
    mapfile -t files < <(relative_to_root "${args[@]}")
else
    mapfile -t files < <(changed_files)
fi
if [ ${#files[@]} -eq 0 ]; then
    echo "clang-format: изменённых C++ файлов нет"
    exit 0
fi

# Сначала --dry-run: он отвечает, какие файлы форматирование меняет, и только их
# имеет смысл называть.
dirty=()
for f in "${files[@]}"; do
    clang-format --dry-run --Werror "$f" >/dev/null 2>&1 || dirty+=("$f")
done

if [ ${#dirty[@]} -eq 0 ]; then
    echo "clang-format: формат чист (${#files[@]} $(plural "${#files[@]}" 'файл' 'файла' 'файлов'))"
    exit 0
fi

if $check; then
    echo "clang-format: не отформатировано файлов — ${#dirty[@]} из ${#files[@]}:"
    printf '%s\n' "${dirty[@]}"
    exit 1
fi

clang-format -i "${dirty[@]}"
echo "clang-format: переформатировано файлов — ${#dirty[@]} из ${#files[@]}:"
printf '%s\n' "${dirty[@]}"
