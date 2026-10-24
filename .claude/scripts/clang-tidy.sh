#!/usr/bin/env bash
# clang-tidy по C++ файлам, изменённым относительно HEAD, или по перечисленным.
#
#   .claude/scripts/clang-tidy.sh -p КАТАЛОГ [--all] [--limit N] [файл...]
#
#   -p КАТАЛОГ  каталог сборки с compile_commands.json
#   --all       все единицы трансляции проекта — то же, что гоняет CI
#   --limit N   сколько замечаний показать, по умолчанию 30
#
# Каталог сборки лучше отладочный (build/Debug): флаги clang-tidy берёт из базы
# компиляции, а в Release добавляется -DNDEBUG, и assert'ы, которыми закреплены
# инварианты, из анализа пропадают. CI анализирует build/Release, поэтому находку,
# которая зависит от NDEBUG, стоит перепроверить с -p build/Release.
#
# Код возврата: 0 — замечаний нет, 1 — есть, 2 — проверить не получилось.
#
# Скрипт зовут руками, когда правка доведена до конца: на недописанном коде находки
# бессмысленны. Вывод сжат до строки на замечание — без фрагментов кода и note:,
# потому что его читают целиком, а важно лишь что нашлось и где.
#
# Проверки те же, что в CI (.clang-tidy), поэтому находка здесь — будущий красный
# пайплайн. Разбираются только настоящие единицы трансляции: заголовок проверяется
# через .cpp, которые его включают.
set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$root" || exit 2

all=false
limit=30
build=""
args=()
while [ $# -gt 0 ]; do
    case "$1" in
    --all) all=true ;;
    -p)
        build="$2"
        shift
        ;;
    --limit)
        limit="$2"
        shift
        ;;
    *) args+=("$1") ;;
    esac
    shift
done

if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy: не найден в PATH"
    exit 2
fi

if [ -z "$build" ]; then
    echo "clang-tidy: не задан каталог сборки, нужен -p КАТАЛОГ"
    exit 2
fi
build_abs="$(cd "$caller" && realpath -m -- "$build")"
build="$build_abs"
shown="${build#"$root/"}"
if [ ! -f "$build/compile_commands.json" ]; then
    echo "clang-tidy: нет $shown/compile_commands.json — сборка не сконфигурирована"
    exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Свои исходники из базы компиляции. Сгенерированное при конфигурации
# лежит в каталоге сборки и отсеивается: правит его не человек, и CI его не смотрит.
jq -r --arg root "$root/" --arg build "$build_abs/" '
    .[]
    | select(.file | startswith($root))
    | select(.file | startswith($build) | not)
    | [.output, .file]
    | @tsv
' "$build/compile_commands.json" > "$tmp/objects"

sources=()
headers=()
if $all; then
    mapfile -t sources < <(cut -f2 "$tmp/objects" | sort -u)
else
    if [ ${#args[@]} -gt 0 ]; then
        mapfile -t files < <(relative_to_root "${args[@]}")
    else
        mapfile -t files < <(changed_files)
    fi
    if [ ${#files[@]} -eq 0 ]; then
        echo "clang-tidy: изменённых C++ файлов нет"
        exit 0
    fi
    for f in "${files[@]}"; do
        case "$f" in
        *.h | *.hpp) headers+=("$root/$f") ;;
        *.cpp) sources+=("$root/$f") ;;
        esac
    done
fi

# Заголовок — не единица трансляции, и отдавать его clang-tidy напрямую нельзя: разобранный
# сам по себе, он живёт в пустоте. Vtable эмитить негде, шаблоны никто не инстанцирует, и
# проверки сообщают о том, чего в настоящей сборке не происходит — например,
# portability-template-virtual-member-function на виртуальном методе шаблона.
# CI гоняет run-clang-tidy по compile_commands.json и заголовков не видит вовсе.
#
# Поэтому изменённый заголовок превращается в те .cpp, которые его включают. Замечания внутри
# самого заголовка при этом никуда не деваются: HeaderFilterRegex в .clang-tidy пропускает
# весь проект.
uncovered=()
if [ ${#headers[@]} -gt 0 ]; then
    if ! command -v ninja >/dev/null 2>&1 || [ ! -f "$build/.ninja_deps" ]; then
        # Молчать нельзя: непроверенный заголовок выглядит как проверенный.
        uncovered=("${headers[@]}")
    else
        printf '%s\n' "${headers[@]}" > "$tmp/headers"

        # Кто кого включает, знает база зависимостей ninja: она пишется по -MD компилятора,
        # то есть по факту препроцессирования, а не по догадке о том, что где написано.
        # Соответствие «объект → исходник» берётся из базы компиляции, а не выводится по
        # списку зависимостей: в базе ninja лежат ещё линковка и timestamp'ы autogen,
        # у которых первая зависимость — случайный заголовок.
        ninja -C "$build" -t deps 2>/dev/null | awk -v objects="$tmp/objects" -v headers="$tmp/headers" -v build="$build_abs/" '
            # Путь к объекту бывает и абсолютным, и относительным каталогу сборки —
            # и в базе компиляции, и в базе ninja, причём независимо. Сравниваем
            # относительные формы.
            function rel(path) {
                return index(path, build) == 1 ? substr(path, length(build) + 1) : path
            }
            BEGIN {
                while ((getline line < objects) > 0) {
                    split(line, field, "\t")
                    source[rel(field[1])] = field[2]
                }
                while ((getline line < headers) > 0)
                    want[line] = 1
            }
            # Начало блока: "<объект>: #deps N, deps mtime ...".
            /^[^ \t]/ {
                mark = index($0, ": #deps")
                object = mark ? rel(substr($0, 1, mark - 1)) : ""
                current = (object != "" && object in source) ? source[object] : ""
                next
            }
            NF && current != "" && ($1 in want) { print $1 "\t" current }
        ' | sort -u > "$tmp/pairs"

        while IFS= read -r source; do
            sources+=("$source")
        done < <(cut -f2 "$tmp/pairs" | sort -u)

        while IFS= read -r header; do
            uncovered+=("$header")
        done < <(comm -23 <(sort -u "$tmp/headers") <(cut -f1 "$tmp/pairs" | sort -u))
    fi
fi

mapfile -t targets < <(printf '%s\n' "${sources[@]}" | grep -v '^$' | sort -u)

# .cpp, которого нет в базе, разбирается с флагами, выведенными по соседнему файлу:
# значит, сборку не переконфигурировали после добавления файла, и проверка ему соврёт.
for f in "${targets[@]}"; do
    grep -qxF "$f" <(cut -f2 "$tmp/objects") ||
        echo "clang-tidy: $shown не знает про ${f#"$root/"} — разобран на выведенных флагах"
done
for f in "${uncovered[@]}"; do
    echo "clang-tidy: ни одна единица трансляции не включает ${f#"$root/"} — не проверен"
done

if [ ${#targets[@]} -eq 0 ]; then
    [ ${#uncovered[@]} -gt 0 ] && exit 2
    echo "clang-tidy: проверять нечего"
    exit 0
fi

# Строка замечания сжимается до «путь:строка:колонка: текст [проверка]»: абсолютный путь
# и пометка -warnings-as-errors ничего не добавляют, а место занимают. Замечания из
# каталога сборки (сгенерированные заголовки) отбрасываются, как и в CI.
printf '%s\n' "${targets[@]}" |
    xargs -r -P "$(nproc)" -I{} clang-tidy -p "$build" --quiet {} 2>/dev/null |
    grep -E ': (warning|error):' |
    grep -vF "$build_abs/" |
    sed -E "s|^$root/||; s/: (warning|error): /: /; s/,-warnings-as-errors\]$/]/" |
    sort -u > "$tmp/findings"

units="${#targets[@]} $(plural "${#targets[@]}" 'единица' 'единицы' 'единиц') трансляции"
count="$(wc -l < "$tmp/findings")"
if [ "$count" -eq 0 ]; then
    echo "clang-tidy: замечаний нет ($units)"
    [ ${#uncovered[@]} -gt 0 ] && exit 2
    exit 0
fi

echo "clang-tidy: $count $(plural "$count" 'замечание' 'замечания' 'замечаний') ($units)"
head -n "$limit" "$tmp/findings"
[ "$count" -gt "$limit" ] && echo "… и ещё $((count - limit)), полный список — с --limit $count"
exit 1
