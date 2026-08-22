# wasafe-vcd — чтение и запись VCD для wasafe

[![CI](https://github.com/gikon3/wasafe-vcd/actions/workflows/ci.yml/badge.svg?branch=main&event=push)](https://github.com/gikon3/wasafe-vcd/actions/workflows/ci.yml)

Библиотека на C++23, которая подключает текстовый формат VCD (IEEE 1364) к
[wasafe](https://github.com/gikon3/wasafe) — backend'у для хранения временных
диаграмм. Ядро wasafe форматов не знает: оно задаёт контракты `Reader` и
`Writer`, а этот проект их реализует.

- **`VcdReader`** разбирает дамп и наполняет любой `Builder` ядра: в память
  (`makeMemoryBuilder()`) или в ленивое блочное хранилище на диске
  (`makeIndexingBuilder()`).
- **`VcdWriter`** выгружает любую `Database` — прочитанную из VCD или
  собранную иначе — обратно в текстовый VCD.

Чтение и запись построены как горячий путь: собственный буфер поверх файла
вместо `iostream`, числа через `std::from_chars`/`std::to_chars`, память не
зависит от размера дампа.

## Пример

```cpp
#include <iostream>
#include <wasafe/io/builder.hpp>
#include <wasafe/io/ingest.hpp>
#include <wasafe/model/database.hpp>
#include <wasafe_vcd/exception.hpp>
#include <wasafe_vcd/reader.hpp>
#include <wasafe_vcd/writer.hpp>

int main(int argc, char** argv) {
    try {
        // Разбор: в память или в ленивое хранилище на диске.
        WaSafe::Vcd::VcdReader reader{argv[1]};
        auto sink = WaSafe::makeMemoryBuilder();  // либо makeIndexingBuilder("dump.wsfstore")
        const WaSafe::Database db = WaSafe::ingest(reader, *sink);

        if (const auto clk = db.find("top.clk"))
            std::cout << clk->valueAt(10).asLogic().toString() << '\n';

        // Выгрузка всей БД обратно в VCD.
        WaSafe::Vcd::VcdWriter{}.write(db, argv[2]);
    }
    catch (const WaSafe::Vcd::Error& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
```

Все ошибки модуля — `WaSafe::Vcd::Error`, наследник `WaSafe::Exception`; код,
который ловит базовый класс ядра, ловит и их. Сообщения начинаются с `vcd:`.

## Чтение

Поддерживается:

- `$scope`/`$upscope` с видами `module`, `task`, `function`, `begin`/`fork`
  (прочие слова читаются как `UNKNOWN`), `$timescale`, `$var` с типами `wire`/`reg`/`integer`/`event`/…,
  `real`/`realtime`, `shortreal`, `string`;
- изменения скаляров, векторов (`b`), вещественных (`r`) и строк (`s`);
- алиасы — несколько `$var` с одним кодом делят один поток;
- секции `$dumpvars`/`$dumpall`/`$dumpon`/`$dumpoff`/`$dumpports` прозрачны: значения внутри
  читаются как обычные изменения; `$comment`, `$date` и `$version` пропускаются;
  переводы строк LF и CRLF;
- значения короче объявленной ширины расширяются по правилу IEEE 1364: нулём
  для `0`/`1`, самим символом для `x`/`z`. Значения длиннее ширины усекаются по
  старшим разрядам;
- одиночные индексы в ссылке (`data[0]`, `mem[3][7:0]`, `m[1][0]`) собираются в
  массивы, в том числе вложенные. Группа остаётся поэлементной, если она
  неоднородна по ширине или размерности, разрежена или если базовое имя уже
  занято обычным сигналом.

Один `VcdReader` можно использовать повторно: состояние разбора живёт внутри
вызова, поэтому `readHeader()` и следом `read()` в другой `Builder` дают
корректный результат.

## Запись

`VcdWriter::write(db, path)` выгружает всю БД:

```text
$version wasafe-vcd 0.1.0 $end
$timescale 1 ns $end
$scope module top $end
$var wire 1 ! clk $end
$var wire 8 " data [7:0] $end
$var wire 1 ! clk_mirror $end
$upscope $end
$enddefinitions $end
#0
0!
b0 "
#10
1!
```

- **Атомарная замена.** Файл пишется во временный `<path>.<pid>.tmp` рядом с
  целевым и подменяет его через `rename` только после успешного завершения.
  Исключение посреди выгрузки оставляет прежний файл целым.
- **Детерминированность.** `$date` не пишется, коды выдаются по порядку обхода,
  изменения с одной меткой времени идут в порядке объявления потоков. Повторная
  выгрузка прочитанного файла даёт тот же текст байт в байт.
- **Алиасы** получают собственный `$var` с общим кодом; изменения потока
  пишутся один раз.
- **Композиты** разворачиваются в плоские `$var` с индексированными именами
  (`mem[3] [7:0]`, `hdr.addr`); при повторном чтении массивы собираются обратно.
- **Векторные литералы усекаются**, если правило расширения восстанавливает
  отброшенное: `00000001 → b1`, `xx001100 → bx001100`, но `00x10000 → b0x10000` —
  один ноль обязан уцелеть перед `x`/`z`, иначе значение дополнилось бы иксами.
- **Вещественные** пишутся в кратчайшей форме с гарантией обратного разбора и
  не зависят от локали.

### Что теряется при выгрузке

Это ограничения формата VCD, а не реализации:

- виды scope без своего слова в VCD (`interface`, `package`, `generate`, …)
  пишутся как `module`, а `fork` после чтения неотличим от `begin`;
- исходное слово `$var` (`reg`, `integer`, …) модель не хранит — logic-типы
  пишутся как `wire`;
- packed-структура со своим потоком пишется одним `$var` и после чтения
  становится вектором;
- строковое значение с пробелом или переводом строки сломает файл: экранирования
  в VCD нет, и значение пишется как есть;
- `$dumpvars` не пишется — начальные значения выходят обычными изменениями на
  первой метке времени.

## Сборка

Нужны C++23, CMake ≥ 3.24 и Conan ≥ 2.28. Зависимость `wasafe/[~0]` должна быть
доступна Conan — например, из локального кэша после `conan create .` или
`conan editable add .` в репозитории wasafe.

```sh
conan build . -s compiler.cppstd=23 --build=missing
```

`-s compiler.cppstd=23` обязателен, если в профиле стоит стандарт ниже:
рецепт проверяет его через `check_min_cppstd`. Сборка через Conan сразу
прогоняет тесты.

Опции CMake:

| Опция | По умолчанию | Что делает |
| --- | --- | --- |
| `WASAFE_VCD_BUILD_TESTS` | `ON` для корневого проекта | тесты (GoogleTest) |
| `WASAFE_VCD_BUILD_BENCHMARKS` | `OFF` | бенчмарк `wasafe-vcd-bench` |
| `WASAFE_VCD_INSTALL` | `ON` для корневого проекта | правила установки и `wasafe_vcd-config.cmake` |

Подключение в CMake:

```cmake
find_package(wasafe_vcd CONFIG REQUIRED)
target_link_libraries(app PRIVATE wasafe::wasafe_vcd)
```

## Тесты

```sh
ctest --test-dir build/Release --output-on-failure
```

Кроме модульных тестов сканера, лексики, кодов и кодировщика значений есть
сквозные: дамп → разбор → `VcdWriter` → повторный разбор, с побайтовой сверкой
текста там, где он важен. Объёмный тест `VcdLarge` гоняет дамп в 24 МиБ через
ленивое хранилище и writer и сверяет значения с формулами генератора, поэтому
эталон не хранится. Тесты должны проходить и под AddressSanitizer: выход за
зарезервированный участок буфера записи видит только он.

## Бенчмарк

```sh
cmake -B build/Release -DWASAFE_VCD_BUILD_BENCHMARKS=ON
cmake --build build/Release --target wasafe_vcd_bench
build/Release/bin/wasafe-vcd-bench --mb=256 --repeat=5 --stage=all
```

Этапы: `parse` — только разбор, `ingest` — разбор с построением хранилища на
диске, `write` — выгрузка уже загруженной БД, `all` — всё. `--keep` оставляет
сгенерированный дамп, `--input=PATH` берёт готовый.

Замер на синтетическом дампе 256 МиБ (256 сигналов, Release, clang), лучший из
пяти прогонов:

| Этап | Скорость |
| --- | --- |
| `parse` | 626 MiB/s |
| `ingest` | 246 MiB/s |
| `write` | 216 MiB/s |

Больше половины времени выгрузки уходит на курсор ядра, который сливает потоки
в общий хронологический порядок; форматирование значений — около трети.
