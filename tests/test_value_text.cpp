#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <wasafe/core/time.hpp>
#include <wasafe/types/logic.hpp>
#include <wasafe/types/logic_vector_view.hpp>

#include "value_buffer.hpp"
#include "value_text.hpp"

using namespace WaSafe;

namespace {

/// Текст вектора через encodeLogic(). Проверяет, что записано ровно width()
/// символов: байт-страж за концом обязан уцелеть.
std::string encode(LogicVectorView v) {
    constexpr char kGuard = '#';
    std::string text(v.width() + 1, kGuard);
    const char* end = Vcd::encodeLogic(text.data(), v);
    EXPECT_EQ(end, text.data() + v.width());
    EXPECT_EQ(text.back(), kGuard);
    text.pop_back();
    return text;
}

/// Литерал после усечения.
std::string trimmed(std::string_view bits) {
    const char* last = bits.data() + bits.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) — длина передана вторым указателем
    return {Vcd::trimmedBegin(bits.data(), last), last};
}

/// Значение, которое reader восстановит из литерала на ширине width.
std::string readBack(std::string_view literal, std::uint32_t width) {
    Vcd::ValueBuffer buffer;
    buffer.decode(literal);
    return encode(buffer.view(width, Vcd::extensionLogic(literal.front())));
}

/// Воспроизводимая строка длины width из символов alphabet.
std::string pattern(std::size_t width, std::string_view alphabet, std::uint32_t seed) {
    std::string bits(width, '0');
    for (char& c : bits) {
        seed = seed * 1'103'515'245U + 12'345U;
        c = alphabet[(seed >> 16) % alphabet.size()];
    }
    return bits;
}

}  // namespace

TEST(ValueText, EncodesScalars) {
    EXPECT_EQ('0', Vcd::scalarChar(Vcd::singleBitView(Logic::ZERO)));
    EXPECT_EQ('1', Vcd::scalarChar(Vcd::singleBitView(Logic::ONE)));
    EXPECT_EQ('x', Vcd::scalarChar(Vcd::singleBitView(Logic::X)));
    EXPECT_EQ('z', Vcd::scalarChar(Vcd::singleBitView(Logic::Z)));
}

// Двухзначные строки: ValueBuffer не хранит b-план, encodeLogic() идёт веткой
// без него. Ширины — по обе стороны границ 64-битных слов.
TEST(ValueText, EncodesWideVectors) {
    for (const std::uint32_t width : {1U, 2U, 63U, 64U, 65U, 127U, 128U, 130U}) {
        for (const std::uint32_t seed : {1U, 2U, 3U}) {
            const auto bits = pattern(width, "01", seed);
            Vcd::ValueBuffer buffer;
            buffer.decode(bits);
            const auto view = buffer.view(width, Logic::ZERO);
            ASSERT_TRUE(view.bval().empty()) << bits;

            EXPECT_EQ(bits, encode(view)) << "width " << width;
        }
    }
}

TEST(ValueText, EncodesFourStateVectors) {
    for (const std::uint32_t width : {1U, 2U, 63U, 64U, 65U, 130U}) {
        for (const std::uint32_t seed : {1U, 2U, 3U}) {
            const auto bits = pattern(width, "01xz", seed);
            Vcd::ValueBuffer buffer;
            buffer.decode(bits);

            EXPECT_EQ(bits, encode(buffer.view(width, Logic::ZERO))) << "width " << width;
        }
    }

    // По одному неопределённому разряду на крайних битах каждого слова.
    std::string bits = pattern(130, "01", 7);
    bits[0] = 'x';    // бит 129, слово 2
    bits[65] = 'z';   // бит 64, слово 1
    bits[66] = 'x';   // бит 63, слово 0
    bits[129] = 'z';  // бит 0, слово 0
    Vcd::ValueBuffer buffer;
    buffer.decode(bits);
    EXPECT_EQ(bits, encode(buffer.view(130, Logic::ZERO)));
}

// Непустой b-план из одних нулей — тоже двухзначное значение.
TEST(ValueText, EncodesZeroBPlane) {
    constexpr std::array<std::uint64_t, 2> aval{0b1010, 0b1};
    constexpr std::array<std::uint64_t, 2> bval{0, 0};
    EXPECT_EQ("01010", encode(LogicVectorView{aval.data(), bval.data(), 5}));
    EXPECT_EQ("1" + std::string(60, '0') + "1010", encode(LogicVectorView{aval.data(), bval.data(), 65}));
    EXPECT_EQ('0', Vcd::scalarChar(LogicVectorView{aval.data(), bval.data(), 1}));
}

TEST(ValueText, TrimsLiterals) {
    constexpr std::array<std::array<std::string_view, 2>, 27> cases{{
            // ведущая единица — без усечения
            {"1", "1"},
            {"1000", "1000"},
            {"1x0z", "1x0z"},
            // нули перед единицей отбрасываются целиком
            {"01", "1"},
            {"0001", "1"},
            {"000101", "101"},
            // перед x/z остаётся один ноль
            {"0x", "0x"},
            {"00x1", "0x1"},
            {"000z", "0z"},
            {"0z0", "0z0"},
            // серия x/z сжимается до одного символа
            {"xxx1", "x1"},
            {"z1", "z1"},
            {"zz0", "z0"},
            {"xxz", "xz"},
            {"xz1", "xz1"},
            {"x0x1", "x0x1"},
            {"x001", "x001"},
            {"zzx0", "zx0"},
            // значение из одного символа
            {"0", "0"},
            {"x", "x"},
            {"z", "z"},
            {"0000", "0"},
            {"xxxx", "x"},
            {"zz", "z"},
            {"11", "11"},
            {"0011", "11"},
            {"xx", "x"},
    }};
    for (const auto& [bits, expected] : cases)
        EXPECT_EQ(expected, trimmed(bits)) << bits;
}

// Правило усечения проверяется правилом чтения: усечённый литерал обязан
// вернуться исходным значением, а литерал без первого символа — уже нет
// (если один символ снять нельзя, то нельзя и больше: снятый префикс был бы
// расширением нового старшего разряда).
TEST(ValueText, TrimmedLiteralsReadBack) {
    std::vector<std::string> values;
    for (const char lead : std::string_view{"01xz"}) {
        for (const std::size_t run : {0U, 1U, 2U, 63U, 64U, 70U}) {
            for (const char next : std::string_view{"01xz"}) {
                for (const std::size_t tail : {0U, 1U, 5U, 64U}) {
                    auto bits = std::string(run, lead) + next +
                            pattern(tail, "01xz", static_cast<std::uint32_t>(run + tail));
                    values.push_back(std::move(bits));
                }
            }
        }
    }

    for (const auto& bits : values) {
        const auto width = static_cast<std::uint32_t>(bits.size());
        const auto literal = trimmed(bits);
        ASSERT_FALSE(literal.empty()) << bits;

        EXPECT_EQ(bits, readBack(literal, width)) << bits << " -> " << literal;
        if (literal.size() > 1)
            EXPECT_NE(bits, readBack(std::string_view{literal}.substr(1), width)) << bits << " -> " << literal;
    }
}

TEST(ValueText, FormatsTime) {
    constexpr std::array<std::pair<TimeStamp, std::string_view>, 5> cases{{
            {0, "0"},
            {1, "1"},
            {-1, "-1"},
            {std::numeric_limits<TimeStamp>::max(), "9223372036854775807"},
            {std::numeric_limits<TimeStamp>::min(), "-9223372036854775808"},
    }};
    for (const auto& [time, expected] : cases) {
        std::array<char, Vcd::kMaxTimeChars> text{};
        const char* end = Vcd::formatTime(text.data(), time);
        EXPECT_EQ(expected, std::string_view(text.data(), end));
    }
}

TEST(ValueText, FormatsRealsRoundTrip) {
    using Limits = std::numeric_limits<double>;
    for (const double value : {0.0, -0.0, 1.0, 0.1, 1.0 / 3.0, -1e-300, -2.2250738585072014e-308, Limits::max(),
                 Limits::lowest(), Limits::min(), Limits::denorm_min(), -Limits::denorm_min()}) {
        std::array<char, Vcd::kMaxRealChars> text{};
        const char* end = Vcd::formatReal(text.data(), value);
        ASSERT_NE(end, text.data()) << value;

        double parsed = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), end, parsed);
        EXPECT_EQ(ec, std::errc{}) << std::string_view(text.data(), end);
        EXPECT_EQ(ptr, end) << std::string_view(text.data(), end);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(value), std::bit_cast<std::uint64_t>(parsed))
                << std::string_view(text.data(), end);
    }
}

TEST(ValueText, FormatsNonFiniteReals) {
    using Limits = std::numeric_limits<double>;
    constexpr std::array<std::pair<double, std::string_view>, 3> cases{{
            {Limits::infinity(), "inf"},
            {-Limits::infinity(), "-inf"},
            {Limits::quiet_NaN(), "nan"},
    }};
    for (const auto& [value, expected] : cases) {
        std::array<char, Vcd::kMaxRealChars> text{};
        const char* end = Vcd::formatReal(text.data(), value);
        EXPECT_EQ(expected, std::string_view(text.data(), end));
    }
}
