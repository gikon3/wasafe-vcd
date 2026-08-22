#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <wasafe/core/time.hpp>
#include <wasafe/model/scope.hpp>
#include <wasafe/types/type.hpp>
#include <wasafe/types/value_view.hpp>

namespace WaSafe::Vcd {

/// Лексика текстового VCD: разбор отдельных токенов заголовка ($scope, $var,
/// $timescale) в типы модели. Всё здесь — чистые функции без состояния и без
/// доступа к сканеру; вызываются раз на объявление, поэтому живут в отдельной
/// единице трансляции и на горячий путь потока значений не влияют.

/// Ссылка из $var: базовое имя + (опционально) разобранный битовый диапазон
/// [msb:lsb]. Хвостовые одиночные индексы отдаются отдельным вектором.
struct Reference {
    std::string_view name;     ///< базовое имя — без индексов и без диапазона
    std::string_view literal;  ///< имя с индексами, но без диапазона
    std::int32_t msb = 0;
    std::int32_t lsb = 0;
    bool hasRange = false;
};

/// Как трактовать изменения значений этого $var.
enum class VarType : std::uint8_t {
    LOGIC,       ///< wire/reg/integer/event/... — всё это биты
    REAL,        ///< real / realtime
    SHORT_REAL,  ///< shortreal
    STRING,      ///< string
};

/// Эффективное описание листового типа $var — уже с решением, доверять ли
/// диапазону из ссылки. Сравнением этих описаний проверяется однородность
/// элементов шины.
struct LeafSpec {
    VarType vt = VarType::LOGIC;
    std::int32_t msb = 0;
    std::int32_t lsb = 0;
    std::uint32_t width = 1;

    friend bool operator==(const LeafSpec&, const LeafSpec&) = default;
};

constexpr std::string_view kEnd = "$end";

/// from_chars поверх string_view: успех только при полном разборе токена.
/// Заголовочная функция — её зовёт и разбор меток времени '#', где вызов на
/// каждый шаг дампа.
template <class T>
[[nodiscard]] bool parseNumber(std::string_view text, T& out) {
    const char* const last = text.data() + text.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) — длина передана вторым указателем
    const auto res = std::from_chars(text.data(), last, out);
    return res.ec == std::errc{} && res.ptr == last;
}

/// Вид VCD-scope -> ScopeKind.
[[nodiscard]] ScopeKind scopeKind(std::string_view t);

[[nodiscard]] VarType varType(std::string_view t);

/// Обёртки секции дампа, прозрачные для разбора: значения внутри читаются как
/// обычные изменения, сами ключевые слова игнорируются. Вызывается только для
/// токенов, у которых tok[1] == 'd'.
[[nodiscard]] bool isDumpKeyword(std::string_view tok);

/// Разобрать масштаб времени из склейки токенов ("1ns", "10ps", ...).
[[nodiscard]] std::optional<TimeScale> parseTimescale(std::string_view s);

/// Разобрать ссылку, срезая справа цепочку `[...]`-групп.
///
/// Диапазон `[msb:lsb]` принимается только самой правой группой: он описывает
/// разряды самого сигнала, тогда как одиночные индексы `[N]` — путь к элементу
/// шины. Всё, что не разобралось целиком, остаётся частью базового имени: "[a:b]"
/// — имя (числа не разобрались), а в "x[1:0][2]" диапазон стоит левее индекса,
/// границами вектора не считается и уходит в имя — база "x[1:0]", элемент [2].
///
/// @param indices сюда уходят одиночные индексы, ВНЕШНЕЕ измерение первым.
[[nodiscard]] Reference parseReference(std::string_view ref, std::vector<std::int32_t>& indices);

[[nodiscard]] LeafSpec leafSpec(VarType vt, std::uint32_t size, const Reference& r);

[[nodiscard]] Type makeLeafType(const LeafSpec& s, ValueKind& kind);

// --- запись: обратные функции ----------------------------------------------

/// ScopeKind -> ключевое слово $scope. Для видов, у которых в VCD нет своего
/// слова, — "module" (лоссовый round-trip, §4.2).
[[nodiscard]] std::string_view scopeKeyword(ScopeKind k);

/// Тип узла -> поле <тип> в $var: "real", "shortreal", "string", "event", иначе "wire".
[[nodiscard]] std::string_view varKeyword(const TypeDescriptor& t);

/// Тип узла -> поле <размер> в $var.
[[nodiscard]] std::uint32_t varSize(const TypeDescriptor& t);

/// Дописать локальное имя узла к ссылке: элемент массива ("[3]") — встык,
/// член структуры ("addr") — через точку.
void appendReferencePart(std::string& ref, std::string_view part);

/// Дописать к ссылке " [msb:lsb]"-диапазон, если он у типа есть.
/// Для EnumType диапазон берётся из базового вектора.
void appendRange(std::string& ref, const TypeDescriptor& t);

}  // namespace WaSafe::Vcd
