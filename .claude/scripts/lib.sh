# Общее для скриптов проверки: подключается через source, сам не запускается.

# Корень репозитория — от расположения скрипта, а не от текущего каталога: скрипт
# зовут из любого места дерева.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Каталог, откуда скрипт позвали: скрипты переходят в корень, а относительные пути
# в аргументах написаны от места вызова.
caller="$PWD"

# C++ файлы, изменённые относительно HEAD, вместе с ещё не добавленными в git.
# Пути — относительно корня; удалённые отсеиваются, их нечего проверять.
changed_files() {
    {
        git -C "$root" diff --name-only --diff-filter=ACMR HEAD -- '*.cpp' '*.h' '*.hpp'
        git -C "$root" ls-files --others --exclude-standard -- '*.cpp' '*.h' '*.hpp'
    } 2>/dev/null | sort -u | while IFS= read -r f; do
        [ -f "$root/$f" ] && printf '%s\n' "$f"
    done
}

# Аргументы командной строки — в пути относительно корня, чтобы вывод не зависел
# от того, откуда скрипт позвали.
relative_to_root() {
    local f abs
    for f in "$@"; do
        abs="$(cd "$caller" && realpath -m -- "$f")"
        printf '%s\n' "${abs#"$root/"}"
    done
}

# Согласование числительного: plural 5 файл файла файлов → «файлов».
plural() {
    local n=$1 one=$2 few=$3 many=$4
    if [ $((n % 100)) -ge 11 ] && [ $((n % 100)) -le 14 ]; then
        printf '%s' "$many"
    elif [ $((n % 10)) -eq 1 ]; then
        printf '%s' "$one"
    elif [ $((n % 10)) -ge 2 ] && [ $((n % 10)) -le 4 ]; then
        printf '%s' "$few"
    else
        printf '%s' "$many"
    fi
}
