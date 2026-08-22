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
[[nodiscard]] inline char scalarChar(LogicVectorView v) {
    const auto a = v.aval()[0] & 1;
    const auto b = v.bval().empty() ? 0 : v.bval()[0] & 1;
    return toChar(logicFromAb(a, b));
}

/// Записать вектор как текст VCD (старший разряд слева) в dst — ровно
/// v.width() символов.
/// @return конец записанного.
[[nodiscard]] inline char* encodeLogic(char* dst, LogicVectorView v) {
    const auto aval = v.aval();
    const auto bval = v.bval();
    if (bval.empty()) {
        for (std::uint32_t i = v.width(); i-- > 0;) {
            const auto wordNo = i / LogicVectorView::kWordWidth;
            const auto bitNo = i % LogicVectorView::kWordWidth;
            *dst++ = toChar(logicFromAb(aval[wordNo] >> bitNo, 0));
        }
    }
    else {
        for (std::uint32_t i = v.width(); i-- > 0;) {
            const auto wordNo = i / LogicVectorView::kWordWidth;
            const auto bitNo = i % LogicVectorView::kWordWidth;
            *dst++ = toChar(logicFromAb(aval[wordNo] >> bitNo, bval[wordNo] >> bitNo));
        }
    }

    return dst;
}

/// Начало усечённого литерала внутри [first, last).
/// Непустой диапазон обязателен; результат всегда < last.
[[nodiscard]] inline const char* trimmedBegin(const char* first, const char* last) {
    const char lead = *first;
    if (lead == '1')
        return first;
    const char* run = first + 1;
    while (run != last && *run == lead)
        ++run;
    if (run == last)
        return last - 1;
    if (lead == '0' && *run == '1')
        return run;
    return run - 1;
}

/// Метка времени в dst; не больше kMaxTimeChars байт.
[[nodiscard]] inline char* formatTime(char* dst, TimeStamp t) {
    return std::to_chars(dst, dst + kMaxTimeChars, t).ptr;
}

/// Вещественное в кратчайшей round-trip форме; не больше kMaxRealChars байт.
[[nodiscard]] inline char* formatReal(char* dst, double v) {
    return std::to_chars(dst, dst + kMaxRealChars, v).ptr;
}

}  // namespace WaSafe::Vcd
