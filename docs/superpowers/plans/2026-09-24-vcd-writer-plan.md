# План реализации: VcdWriter

Дата: 2026-09-24
Спека: [2026-09-16-vcd-writer-design.md](../specs/2026-09-16-vcd-writer-design.md)

Шаг 1 спеки (`Emitter` и его тесты) уже в рабочем дереве. План расписывает
шаги 2–6: для каждой единицы — её заголовок и что делает каждый метод. Тела
методов здесь не пишутся.

## 0. Карта файлов

| Файл | Действие | Содержимое |
| --- | --- | --- |
| `src/vcd_syntax.hpp/.cpp` | дополнить | обратные функции лексики: ключевые слова, размер, ссылка |
| `src/value_text.hpp` | новый, только заголовок | бит-планы → текст, усечение, числа через `to_chars` |
| `src/code_book.hpp` | новый, только заголовок | `CodeBook`: `SignalId` → код, порядковый номер → код |
| `src/writer.cpp` | переписать | `Dumper` в анонимном namespace, `VcdWriter::write()` |
| `src/CMakeLists.txt` | не трогать | новых `.cpp` нет |
| `tests/test_value_text.cpp` | новый | тесты `value_text.hpp` |
| `tests/test_code_book.cpp` | новый | тесты `CodeBook` |
| `tests/test_vcd_writer.cpp` | новый | тесты лексики и сквозные тесты writer'а |
| `tests/vcd_test_util.hpp` | новый | `kVcd`, `TempVcd`, `parseToMemory()`, `checkContents()` — общие для `test_vcd.cpp` и `test_vcd_writer.cpp` |
| `tests/test_vcd.cpp` | дополнить | хелперы уходят в `vcd_test_util.hpp` |
| `tests/test_vcd_large.cpp` | дополнить | объёмный round-trip |
| `tests/CMakeLists.txt` | дополнить | `test_value_text.cpp`, `test_code_book.cpp`, `test_vcd_writer.cpp` |
| `bench/bench_vcd.cpp` | дополнить | `--stage=write` |

Все внутренние заголовки лежат в `namespace WaSafe::Vcd`, как остальные в `src/`.

## 1. vcd_syntax: обратные функции

Всё это вызывается один раз на `$scope`/`$var`, поэтому объявления идут в
заголовок, а тела — в `vcd_syntax.cpp`, рядом с прямыми функциями, которым они
обратны.

```cpp
// --- запись: обратные функции ----------------------------------------------

/// ScopeKind -> ключевое слово $scope. Обратная к scopeKind(). Для видов, у
/// которых в VCD нет своего слова, — "module" (лоссовый round-trip, §4.2).
[[nodiscard]] std::string_view scopeKeyword(ScopeKind k);

/// Тип узла -> поле <тип> в $var: "real", "shortreal", "string", "event", иначе "wire".
[[nodiscard]] std::string_view varKeyword(const TypeDescriptor& t);

/// Тип узла -> поле <размер> в $var.
[[nodiscard]] std::uint32_t varSize(const TypeDescriptor& t);

/// Дописать локальное имя узла к ссылке: элемент массива ("[3]") — встык,
/// член структуры ("addr") — через точку. Та же склейка, что в
/// Hierarchy::fullPath(), но без пути по scope.
void appendReferencePart(std::string& ref, std::string_view part);

/// Дописать к ссылке " [msb:lsb]"-диапазон, если он у типа есть.
/// Для EnumType диапазон берётся из базового вектора.
void appendRange(std::string& ref, const TypeDescriptor& t);
```

**`scopeKeyword()`** — `switch` по `ScopeKind`: `MODULE → "module"`,
`TASK → "task"`, `FUNCTION → "function"`, `BLOCK → "begin"`, все остальные
(включая `ROOT` и `UNKNOWN`) → `"module"`. `ROOT` сюда в норме не приходит —
`Dumper` для корня `$scope` не пишет, — но функция тотальная и не бросает.

**`varKeyword()`** — `switch` по `t.kind()`: `REAL → "shortreal"` при
`RealType::isShortreal()`, иначе `"real"`; `STRING → "string"`,
`EVENT → "event"`, `default → "wire"`.

**`varSize()`** — `REAL → 32` для shortreal и `64` для real, `STRING → 1`,
`EVENT → 1`, иначе `t.bitWidth()`. Для packed-композита `bitWidth()` уже даёт
полную ширину потока, отдельной ветки не нужно. Это же число `Dumper` хранит
как ширину потока (§4.3), поэтому для `EVENT`, у которого `bitWidth()` может
быть 0, объявление и форма записи всё равно совпадают.

**`appendReferencePart()`** — пустой `part` пропустить; если `ref` пуст,
дописать `part` как есть; иначе если `part` начинается с `'['` — дописать
встык; иначе дописать `'.'` и `part`. Имена членов структуры в иерархии
хранятся без точки (см. `Hierarchy::fullPath()`), поэтому точку ставит склейка.

**`appendRange()`** — для `ENUM` источником диапазона служит базовый тип; дальше
только для вектора с `bitWidth() > 1`: взять `msb()`/`lsb()` и дописать
`[msb:lsb]` (числа через
`std::to_chars` в локальный массив — `std::format` не нужен, но и не запрещён:
здесь не горячий путь). Между именем и диапазоном — пробел: так пишут
симуляторы, а `Parser::parseVar()` склеивает токены ссылки до `$end`. Скаляры,
real, string, packed-композиты — без диапазона: reader восстановит `[w-1:0]`.

## 2. value_text.hpp: значения в текст

Только заголовок, всё `inline`: функции зовутся на каждое изменение. Зеркало
`value_buffer.hpp` — там текст → бит-планы, здесь обратно.

```cpp
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <wasafe/core/time.hpp>
#include <wasafe/types/logic.hpp>
#include <wasafe/types/logic_vector_view.hpp>

namespace WaSafe::Vcd {

/// Верхняя оценка длины TimeStamp в тексте: "-9223372036854775808".
inline constexpr std::size_t kMaxTimeChars = 20;
/// Верхняя оценка длины double в кратчайшей форме to_chars — с запасом от
/// max_digits10 плюс знак, точка и экспонента.
inline constexpr std::size_t kMaxRealChars = 32;

/// Символ однобитового значения ('0', '1', 'x', 'z').
[[nodiscard]] inline char scalarChar(LogicVectorView v);

/// Записать вектор как текст VCD (старший разряд слева) в dst — ровно
/// v.width() символов.
/// @return конец записанного.
[[nodiscard]] inline char* encodeLogic(char* dst, LogicVectorView v);

/// Начало усечённого литерала внутри [first, last) по правилу §3.4 спеки.
/// Непустой диапазон обязателен; результат всегда < last.
[[nodiscard]] inline const char* trimmedBegin(const char* first, const char* last);

/// Метка времени в dst; не больше kMaxTimeChars байт.
[[nodiscard]] inline char* formatTime(char* dst, TimeStamp t);

/// Вещественное в кратчайшей round-trip форме; не больше kMaxRealChars байт.
[[nodiscard]] inline char* formatReal(char* dst, double v);

}  // namespace WaSafe::Vcd
```

**`scalarChar()`** — `a = v.aval()[0] & 1`, `b = v.bval().empty() ? 0 : v.bval()[0] & 1`,
вернуть `toChar(logicFromAb(a, b))`. Мимо цикла `encodeLogic()`: скаляры —
самые частые изменения в дампе.

**`encodeLogic()`** (шаг 3, простой вариант) — взять `aval()`/`bval()` один раз;
пустой `bval()` означает «все b-биты нули», и тогда весь вектор идёт веткой без
чтения b-плана. Цикл по разрядам от `width - 1` до `0`: слово `i / kWordWidth`,
бит `i % kWordWidth`, символ через `toChar(logicFromAb(a, b))`. Индексы слов
берутся из `span` без проверок, `operator[]` у `LogicVectorView` не используется.

(Шаг 6, по результатам бенчмарка.) Для слова без b-битов — развёртка по восемь
разрядов: байт `aval` размножается на восемь байт и маскируется до `'0'`/`'1'`,
восемь символов пишутся одним `memcpy` — операция, обратная
`packTwoStateChunk()`. Константы добавляются рядом с `kByteGather` в
`value_buffer.hpp` или здесь, с `static_assert` на little-endian, как
`kSwarUsable`. Сигнатура `encodeLogic()` не меняется.

**`trimmedBegin()`** — `c = *first`, `run` — конец ведущей серии из `c`:

- `c == '1'` → `first` (усечения нет);
- серия дошла до `last` (значение из одного символа) → `last - 1`;
- `c == 'x'` или `'z'` → `run - 1` (от серии остаётся один символ);
- `c == '0'`, `*run == '1'` → `run` (нули отбрасываются целиком);
- `c == '0'`, `*run` — `'x'`/`'z'` → `run - 1` (один ноль уцелевает).

Функция работает на уже закодированном тексте, а не на бит-планах: так правило
тестируется на строках, в отрыве от упаковки. Цена — один `memmove` на
изменение в `Dumper`; если бенчмарк покажет его заметным, усечение переносится
на бит-планы (поиск старшего значимого разряда), а сигнатура остаётся.

**`formatTime()`** — `std::to_chars(dst, dst + kMaxTimeChars, t)`, вернуть `ptr`.

**`formatReal()`** — `std::to_chars(dst, dst + kMaxRealChars, v)` без формата и
точности: это и есть кратчайшее round-trip представление. `inf`/`nan` выходят
как `inf`/`-inf`/`nan` — `from_chars` в reader'е их принимает.

## 3. code_book.hpp: коды VCD

Только заголовок: `code()` вызывается на каждое изменение.

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include <wasafe/core/ids.hpp>

namespace WaSafe::Vcd {

/// Коды VCD для потоков выгружаемой БД. Код выдаётся на поток (SignalId), а не
/// на узел: алиасы получают код первого объявления. Горячий путь — code() по
/// порядковому номеру потока, который совпадает с индексом source в курсоре.
class CodeBook final {
public:
    /// Результат assign(): код потока и его порядковый номер.
    struct Assignment {
        std::string_view code;  ///< валиден до следующего assign()
        std::uint32_t ordinal = 0;
        bool fresh = false;  ///< поток встретился впервые — код только что выдан
    };

public:
    /// Заранее развернуть арену и таблицы под ожидаемое число потоков.
    void reserve(std::size_t streams);

    /// Код потока: существующий, если поток уже встречался (алиас), иначе
    /// следующий по порядку.
    [[nodiscard]] Assignment assign(SignalId id);

    /// Код по порядковому номеру потока. Без проверок: ordinal < size().
    [[nodiscard]] std::string_view code(std::uint32_t ordinal) const noexcept {
        return {arena_.data() + offsets_[ordinal], offsets_[ordinal + 1] - offsets_[ordinal]};
    }

    /// Число выданных кодов (уникальных потоков).
    [[nodiscard]] std::size_t size() const noexcept { return offsets_.size() - 1; }

private:
    /// Записать код с номером ordinal в dst; вернуть длину. Схема та же, что у
    /// VcdGen::code(): цифры в основании 94 от младшей, алфавит '!'..'~'.
    static std::size_t encode(std::uint32_t ordinal, char* dst);

private:
    static constexpr std::int32_t kNone = -1;
    static constexpr std::uint32_t kAlphabet = '~' - '!' + 1;  ///< 94
    static constexpr std::size_t kMaxCodeChars = 5;            ///< 94^5 > 2^32

private:
    std::vector<char> arena_;                  ///< коды подряд, без разделителей
    std::vector<std::uint32_t> offsets_{0};    ///< code(i) = arena_[offsets_[i], offsets_[i+1])
    std::vector<std::int32_t> ordinalById_;    ///< SignalId::get() -> ordinal либо kNone
};

}  // namespace WaSafe::Vcd
```

`code()` и `size()` — однострочники, поэтому их тела даны прямо в заголовке.

**`reserve()`** — `arena_.reserve(streams * 3)` (коды до 94² потоков занимают
не больше двух байт), `offsets_.reserve(streams + 1)`. `ordinalById_` не
трогается: его размер задаёт максимальный id, а не число потоков.

**`assign()`**:

1. если `id.get() >= ordinalById_.size()` — `resize(id.get() + 1, kNone)`;
2. если `ordinalById_[id.get()] != kNone` — вернуть `{code(o), o, false}`;
3. иначе `o = size()`; дописать в арену `encode(o, ...)` (через временный
   `char[kMaxCodeChars]` и `insert`), `offsets_.push_back(arena_.size())`,
   запомнить `o` в `ordinalById_`, вернуть `{code(o), o, true}`.

Невалидный `SignalId` сюда не приходит: `Dumper` зовёт `assign()` только для
узлов с `hasOwnStream()`, у которых `streamId()` есть.

**`encode()`** — цикл `do { *dst++ = '!' + v % 94; v /= 94; } while (v != 0)`
на копии `ordinal`, та же последовательность, что у `VcdGen::code()`. Совпадение
с генератором не обязательно для корректности, но позволяет сверять вывод
побайтово с тем, что генерирует тест.

## 4. writer.cpp: Dumper

```cpp
#include "wasafe_vcd/writer.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <wasafe/core/time.hpp>
#include <wasafe/model/database.hpp>

#include "code_book.hpp"
#include "emitter.hpp"
#include "value_text.hpp"
#include "vcd_syntax.hpp"
#include "wasafe_vcd/version.hpp"

namespace WaSafe::Vcd {

namespace {

// ---------------------------------------------------------------------------
// Dumper
// ---------------------------------------------------------------------------
/// Выгрузка одной Database в один файл: заголовок, иерархия и поток значений.
/// Создаётся на каждый вызов write(), поэтому состояние заведомо чистое.
class Dumper final {
public:
    Dumper(const Database& db, const std::filesystem::path& path);

    void run();

private:
    void writeHeader();

    /// Содержимое scope: сначала $var своих сигналов, затем вложенные scope.
    void writeScopeBody(const Scope& scope);
    /// $scope ... $upscope вокруг writeScopeBody().
    void writeScope(const Scope& scope);
    /// Узел сигнала: $var, если у него свой поток, иначе спуск в children().
    void declareSignal(const Signal& signal);
    void writeVar(const TypeDescriptor& type, SignalId id);

    void writeValues();
    void writeTime(TimeStamp t);
    void writeChange(std::uint32_t ordinal, const ValueView& value);
    void writeScalar(std::string_view code, LogicVectorView v);
    void writeVector(std::string_view code, LogicVectorView v);
    void writeReal(std::string_view code, double v);
    void writeString(std::string_view code, std::string_view v);

    void appendStream(std::uint32_t ordinal, SignalId id, std::uint32_t size);

private:
    const Database& db_;
    Emitter out_;
    CodeBook codes_;

    // Параллельные массивы на уникальные потоки в порядке первого появления;
    // индекс — порядковый номер потока, он же source курсора.
    std::vector<SignalId> streams_;
    std::vector<std::uint32_t> widths_;

    /// Ссылка текущего узла: локальные имена от верхнего уровня scope вниз.
    /// Растёт и откатывается по ходу обхода, аллокаций на узел нет.
    std::string ref_;
};

}  // namespace

void VcdWriter::write(const Database& db, const std::filesystem::path& path) {
    Dumper{db, path}.run();
}

}  // namespace WaSafe::Vcd
```

Строки заголовка и иерархии собираются через `std::format`: здесь не горячий
путь, а запрет из спеки касается только форматирования `std::filesystem::path`.
Поток значений пишется указателем в участок из `reserve()` (§4.4).

### 4.1. Конструктор и run()

**`Dumper(db, path)`** — запомнить `db`, построить `out_{path}` (он же откроет
временный файл и бросит `Vcd::Error`, если не вышло). Больше ничего: никакой
работы с БД в конструкторе.

**`run()`**:

1. `writeHeader()`;
2. `writeScopeBody(db_.root())` — корень без `$scope`/`$upscope`;
3. `out_.text("$enddefinitions $end\n")`;
4. `writeValues()`;
5. `out_.finish()`.

Исключение на любом шаге до `finish()` уходит наружу, а деструктор `Emitter`
удаляет временный файл — целевой не трогается.

### 4.2. Заголовок

**`writeHeader()`** — две строки через `out_.text()`:

- `std::format("$version wasafe-vcd {} $end\n", Version::kString)`;
- `"$timescale " + formatTimeScale(db_.timeScale()) + " $end\n"`.

`$date` не пишется (§4.1 спеки).

### 4.3. Иерархия

**`writeScopeBody(scope)`**:

1. для каждого `Signal` из `scope.signals()` — `assert(ref_.empty())`,
   `declareSignal(signal)`;
2. для каждого `Scope` из `scope.scopes()` — `writeScope(child)`.

Порядок «сначала сигналы, потом вложенные scope» задан §4.2 спеки.

`ref_` на входе в тело любого scope пуст: `declareSignal()` откатывает всё, что
дописал, а `writeScope()` в `ref_` не пишет — путь по scope выражен
вложенностью `$scope`. Поэтому `assert`, а не `clear()`: нарушение инварианта
должно ловиться, а не маскироваться.

**`writeScope(scope)`** — `std::format("$scope {} {} $end\n",
scopeKeyword(scope.kind()), scope.name())`, затем `writeScopeBody(scope)`, затем
`"$upscope $end\n"`. Рекурсия по глубине иерархии scope — она небольшая.

**`declareSignal(signal)`**:

1. `mark = ref_.size()`; `appendReferencePart(ref_, signal.name())`;
2. если `signal.streamId()` есть — узел объявляется: `type = signal.type()`,
   `appendRange(ref_, *type)`, `writeVar(*type, *id)`. В дети не спускаться:
   члены packed-композита — срезы того же потока (§4.3 спеки);
3. иначе — `declareSignal(child)` для каждого `child` из `signal.children()`;
4. `ref_.resize(mark)`.

`streamId()` вместо пары `hasOwnStream()` + `*streamId()`: проверка одна и та
же, а узел ищется один раз.

Диапазон дописывается прямо в `ref_`: шаг 4 откатывает и локальное имя, и
диапазон, временная строка не нужна. Запись и откат стоят в одной функции,
поэтому связь между ними не скрыта.

Отдельный `leafNodes()` не зовётся: обход сам находит листья и сам же
дедуплицирует алиасы через `CodeBook`.

**`writeVar(type, id)`**:

1. `a = codes_.assign(id)`;
2. `size = varSize(type)`;
3. если `a.fresh` — `appendStream(a.ordinal, id, size)`;
4. `std::format("$var {} {} {} {} $end\n", varKeyword(type), size, a.code, ref_)`.

Размер считается один раз и уходит и в `$var`, и в `widths_`: выбор формы
изменения в `writeChange()` идёт по тому же числу, что объявлено в файле.
`bitWidth()` для этого не годится — у `EVENT`, `REAL` и `STRING` он не
совпадает с размером в `$var`.

Код в строку `$var` пишется сразу из `a.code`: вид валиден до следующего
`assign()`, а он будет только на следующем узле.

**`appendStream(ordinal, id, size)`** — `assert(ordinal == streams_.size())`,
затем `push_back` в `streams_` и `widths_`.

Номера совпадают по построению: `CodeBook::assign()` выдаёт новому потоку
`ordinal = size()`, а `Dumper` дописывает в массивы ровно тогда, когда
`a.fresh`. Совпадение обязательно: курсор отдаёт `source` как индекс в
`streams_`, а `writeChange()` по нему же берёт код и ширину — разъехавшиеся
номера дали бы значения одного потока под кодом другого в синтаксически
верном файле. `resize` вместо `assert` молча принял бы рассинхронизацию и
отдал бы в курсор дыры из `SignalId{}`.

### 4.4. Поток значений

**`writeValues()`**:

1. если `streams_` пуст — вернуться (писать нечего, курсор не открывается);
2. `cursor = db_.storage().openCursor(streams_, kWholeTime)`;
3. `haveTime = false`, `now = kNoTime`;
4. пока `cursor->next()`: `c = cursor->current()`; если `!haveTime || c.time != now`
   — `writeTime(c.time)`, `now = c.time`, `haveTime = true`; затем
   `writeChange(c.source, c.value)`.

Флаг вместо сравнения с `kNoTime`: `kNoTime` — это `INT64_MIN`, формально
допустимая метка. `kWholeTime` — полуоткрытый `[INT64_MIN, INT64_MAX)`, так что
изменение ровно на `INT64_MAX` не выгружается; на практике таких меток нет.

**`writeTime(t)`** — `p = out_.reserve(1 + kMaxTimeChars + 1)`, `*p++ = '#'`,
`p = formatTime(p, t)`, `*p++ = '\n'`, `out_.commit(p)`.

**`writeChange(ordinal, value)`** — `code = codes_.code(ordinal)`, затем по
`value.kind()`:

- `LOGIC` — `widths_[ordinal] == 1` → `writeScalar()`, иначе `writeVector()`;
- `REAL` → `writeReal()`;
- `STRING` → `writeString()`;
- `NONE` (и всё прочее) — пропустить.

Выбор скаляр/вектор делается по объявленной ширине потока, а не по ширине
значения: объявление в `$var` и форма записи обязаны совпадать.

Вид берётся из самого значения, отдельного массива видов нет: проверка
`value.kind()` нужна в любом случае — значение вида `NONE` надо пропустить, а
`logic()`/`real()` на нём бросили бы `bad_variant_access`.

Каждая из четырёх функций ниже делает **один** `reserve()` по оценке из
таблицы §4.4 спеки, пишет указателем и один раз зовёт `commit()`. Код
копируется `std::memcpy(p, code.data(), code.size())`.

**`writeScalar(code, v)`** — `reserve(1 + code.size() + 1)`;
`scalarChar(v)`, код, `'\n'`.

**`writeVector(code, v)`** — `reserve(1 + v.width() + 1 + code.size() + 1)`:
оценка по фактической ширине значения, потому что `encodeLogic()` пишет ровно
`v.width()` символов. `'b'`; `end = encodeLogic(p, v)`;
`keep = trimmedBegin(p, end)`; если `keep != p` — `sz = end - keep`,
`memmove(p, keep, sz)`, `end = p + sz`; затем `' '`, код, `'\n'`.

**`writeReal(code, v)`** — `reserve(1 + kMaxRealChars + 1 + code.size() + 1)`;
`'r'`, `formatReal()`, `' '`, код, `'\n'`.

**`writeString(code, v)`** — `reserve(1 + v.size() + 1 + code.size() + 1)`;
`'s'`, текст как есть (экранирования нет, §4.4 спеки), `' '`, код, `'\n'`.

Порядок источников при совпадении времени задаёт курсор (порядок `streams_`),
поэтому вывод детерминирован и сравним побайтово.

## 5. Тесты

### 5.1. tests/test_value_text.cpp — value_text.hpp

- `EncodesScalars` — `scalarChar()` на `0`, `1`, `x`, `z` (виды через
  `singleBitView()` из `value_buffer.hpp`).
- `EncodesWideVectors` — `encodeLogic()` на ширинах 1, 63, 64, 65, 130: значение
  из `ValueBuffer::decode()` известной строки кодируется обратно в ту же строку.
  Это заодно сверка с reader'ом.
- `EncodesFourStateVectors` — то же на строках с `x`/`z` в разных словах.
- `TrimsLiterals` — `trimmedBegin()` на строках: `"0001" → "1"`,
  `"00x1" → "0x1"`, `"000z" → "0z"`, `"xxx1" → "x1"`, `"zz0" → "z0"`,
  `"1000" → "1000"`, `"0000" → "0"`, `"xxxx" → "x"`, `"1" → "1"`.
- `TrimmedLiteralsReadBack` — для набора строк: усечь, прогнать через
  `ValueBuffer::decode()` + `view(width, extensionLogic(msb))` и сравнить с
  исходным неусечённым значением. Проверяет правило через само правило чтения.
- `EncodesZeroBPlane` — непустой, но нулевой `bval()` кодируется так же, как
  пустой.
- `FormatsTime` — `formatTime()` на `0`, `INT64_MIN`, `INT64_MAX` (длина ≤ 20).
- `FormatsRealsRoundTrip` — `formatReal()` на нулях обоих знаков, `0.1`, `1/3`,
  денормалях и крайних значениях: `from_chars` возвращает побитово то же число.
- `FormatsNonFiniteReals` — `inf`, `-inf`, `nan`.

Шаг 6 добавляет сюда `PackedExpansionMatchesBitwise` — сверку быстрого пути
`encodeLogic()` с побитовым эталоном на случайных векторах.

### 5.2. tests/test_code_book.cpp и tests/test_vcd_writer.cpp

`CodeBook` — в `test_code_book.cpp`:

- `MatchesGenerator` — первые ~10 000 кодов `assign()` совпадают с
  `VcdGen::code(i)`, включая переход на двухбайтовые коды на 94.
- `NumbersStreamsInOrderOfAppearance` — номера идут 0, 1, 2… в порядке первых
  `assign()`, независимо от значений `SignalId`.
- `ReusesCodeForAlias` — повторный `assign()` того же `SignalId` возвращает тот
  же код и номер с `fresh == false`; `size()` не растёт.
- `ReserveCoversWholeArena` — после `reserve()` арена не переаллоцируется.

Лексика — в `test_vcd_writer.cpp`, набор `VcdSyntaxWrite`: обращение
`scopeKeyword()`, `varKeyword()`/`varSize()` для битовых и небитовых типов,
склейка `appendReferencePart()`, `appendRange()` с крайними границами, enum и
обратным разбором через `parseReference()`.

Сквозные — там же, набор `VcdWriter`. Схема у всех одна: VCD-строка →
временный файл → разбор в память → `VcdWriter::write()` → текст выгрузки и её
повторный разбор (хелпер `roundTrip()`). Общие с `test_vcd.cpp` хелперы
(`kVcd`, `TempVcd`, `parseToMemory()`, `checkContents()`) лежат в
`tests/vcd_test_util.hpp`.

- `RoundTripsHierarchyAndValues` — `kVcd` после выгрузки проходит
  `checkContents()`, порядок сигналов и вложенный scope сохраняются; повторная
  выгрузка прочитанного даёт тот же текст.
- `RoundTripsScopeKindsAndRootSignals` — `task`/`function`/`begin` сохраняют
  вид, сигнал корня пишется до первого `$scope` и остаётся в корне.
- `WritesScalarsVectorsRealsAndStrings` — побайтовая сверка всего файла:
  заголовок, все четыре формы, `event` как `wire 1`, диапазоны `[8:1]` и
  `[0:3]`; повторное чтение восстанавливает значения и границы векторов.
- `SharesOneCodeBetweenAliases` — два `$var` с одним кодом, изменения потока
  записаны один раз.
- `FlattensArraysIntoIndexedNames` — `mem[0] [7:0]`, `bits[0]`, `m[1][1] [3:0]`
  на выходе и обратное слияние в массивы при повторном чтении.
- `TrimsLeadingRunsButKeepsOnes` — все четыре случая §3.4 спеки плюс значения
  из одних нулей и одних иксов: текст литерала и значение после чтения.
- `WritesRealsIndependentlyOfLocale` — при `LC_NUMERIC` с запятой real
  пишется с точкой в кратчайшей форме и читается в то же число.

### 5.3. tests/test_vcd_large.cpp

`RoundTripsLargeDump` — дамп `VcdGen` разбирается в ленивое хранилище,
выгружается `VcdWriter`, выгрузка разбирается в память и сверяется с формулами
генератора. Проверка вынесена в `expectMatchesGenerator()`, общую с
`IngestRoundTripMatchesGenerator`. Ленивое хранилище на входе выбрано
намеренно: курсор writer'а проходит потоки через границы блоков, а коды выходят
за один байт.

## 6. Бенчмарк

`bench/bench_vcd.cpp`: допустимые значения `--stage` —
`parse|ingest|write|all`. Этап `write`: сгенерированный файл один раз
разбирается в `MemoryBuilder` вне замера, затем `--repeat` раз замеряется
`VcdWriter::write()` во временный файл; MiB/s считаются по размеру выходного
файла, печать — в том же формате, что у существующих этапов. `all` включает
`write`.

## 7. Порядок и критерии готовности

| Шаг | Что | Готово, когда |
| --- | --- | --- |
| 2a | `vcd_syntax` + модульные тесты лексики | тесты зелёные |
| 2b | `CodeBook` + его тесты | тесты зелёные |
| 2c | `Dumper`: заголовок, иерархия, `$var`; `writeValues()` пустой | `SharesOneCodeBetweenAliases`, `FlattensArraysIntoIndexedNames` проходят по тексту заголовка |
| 3 | `value_text.hpp` (побитовый `encodeLogic`) + поток значений | тесты §5.1 и остальные из §5.2 зелёные |
| 4 | объёмный round-trip | `test_vcd_large` зелёный, в том числе в ASan-профиле |
| 5 | `--stage=write` | бенчмарк печатает MiB/s |
| 6 | пакетная развёртка | только если профиль показывает `encodeLogic()` в топе; сверочный тест зелёный. По замеру §7.1 — не делается |

Каждый шаг — отдельный коммит. После шагов 1–4 writer рабочий.

### 7.1. Замер после шага 5 и решение по шагу 6

Замер 2026-10-16: Release, clang, синтетический дамп `VcdGen` 256 МиБ
(256 потоков, 67 328 меток времени), лучший из 5 прогонов.

| Этап | Время | Скорость |
| --- | --- | --- |
| `parse-only` | 0,41 с | 626 MiB/s |
| `ingest` | 1,04 с | 246 MiB/s |
| `write` | 1,19 с | 216 MiB/s (256 МиБ на выходе) |

Профиль этапа `write` (`perf record`, доли от времени выгрузки без
подготовительного разбора):

| Что | Доля |
| --- | --- |
| курсор ядра: `MergeCursor::takeMin/next/advanceLast`, `MemoryCursor::next`, `TimeColumn`, `ColumnView` | ~55% |
| `Dumper` целиком (встроен в `VcdWriter::write`) | ~35% |
| — побитовый цикл `encodeLogic()` | ~15% |
| — загрузка `aval`/`bval` в `scalarChar()` | ~12% |

**Шаг 6 не делается.** Условие из таблицы выше — `encodeLogic()` в топе
профиля — не выполнено: даже многократное ускорение цикла даст около 10–12%
выгрузки (~240 MiB/s). Загрузки в `scalarChar()` — первое обращение к данным
значения из курсора, то есть промахи кеша; они переедут в любую реализацию
кодировщика. Основная стоимость — слияние 256 потоков в `MergeCursor` ядра
(`takeMin()` — около четверти выгрузки). Симметрию с reader'ом по скорости
даёт ускорение курсора в `wasafe`, а не в этом проекте.

## 8. Решённые вопросы к спеке

1. **Enum поверх вектора.** `appendRange()` берёт диапазон из базового типа
   `EnumType`, поэтому `ENUM` поверх `logic [8:1]` возвращается с теми же
   границами (§1).
2. **shortreal.** `varKeyword()` пишет `shortreal` при
   `RealType::isShortreal()`, `varSize()` — 32 (§1).
3. **Массив видов потоков.** Не заводится: вид берётся из `ValueView::kind()`,
   проверка которого нужна в любом случае ради `NONE` (§4.4). Спека §3.6
   говорит о двух параллельных массивах.
