#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <wasafe/core/ids.hpp>

#include "code_book.hpp"
#include "vcd_gen.hpp"

using namespace WaSafe;

// Диапазон захватывает переходы на двух- (94) и трёхсимвольные (94^2) коды.
// Код сверяется и сразу после выдачи, и после всех assign(): рост арены не
// должен сдвигать уже выданные коды.
TEST(CodeBook, MatchesGenerator) {
    constexpr std::uint32_t kStreams = 94 * 94 + 200;
    Vcd::CodeBook codes;
    codes.reserve(kStreams);
    for (std::uint32_t i = 0; i < kStreams; ++i) {
        const auto a = codes.assign(SignalId{i});
        ASSERT_EQ(a.code, VcdGen::code(i)) << i;
        ASSERT_EQ(a.ordinal, i);
        ASSERT_TRUE(a.fresh) << i;
    }
    ASSERT_EQ(codes.size(), kStreams);

    for (std::uint32_t i = 0; i < kStreams; ++i)
        ASSERT_EQ(codes.code(i), VcdGen::code(i)) << i;
}

// Номер потока — порядок первого assign(), а не значение SignalId: id в базе
// разрежены и идут не в порядке обхода.
TEST(CodeBook, NumbersStreamsInOrderOfAppearance) {
    constexpr std::array<std::uint32_t, 5> kIds{1000, 5, 70'000, 0, 6};
    Vcd::CodeBook codes;
    for (std::uint32_t i = 0; i < kIds.size(); ++i) {
        const auto a = codes.assign(SignalId{kIds[i]});
        EXPECT_EQ(a.ordinal, i) << "id " << kIds[i];
        EXPECT_EQ(a.code, VcdGen::code(i)) << "id " << kIds[i];
        EXPECT_TRUE(a.fresh) << "id " << kIds[i];
    }
    EXPECT_EQ(codes.size(), kIds.size());
}

TEST(CodeBook, ReusesCodeForAlias) {
    Vcd::CodeBook codes;
    const auto first = codes.assign(SignalId{42});
    const std::string firstCode{first.code};  // вид валиден только до следующего assign()
    const auto firstOrdinal = first.ordinal;
    std::ignore = codes.assign(SignalId{7});

    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto alias = codes.assign(SignalId{42});
        EXPECT_EQ(alias.code, firstCode);
        EXPECT_EQ(alias.ordinal, firstOrdinal);
        EXPECT_FALSE(alias.fresh);
    }
    EXPECT_EQ(codes.size(), 2U);

    // Следующий новый поток получает следующий номер, алиасы его не сдвигают.
    const auto next = codes.assign(SignalId{3});
    EXPECT_EQ(next.ordinal, 2U);
    EXPECT_EQ(next.code, VcdGen::code(2));
    EXPECT_TRUE(next.fresh);
}

// После reserve(n) первые n assign() укладываются в арену без перевыделения, и
// код первого потока остаётся на месте. Числа потоков стоят по обе стороны
// границ длины кода, включая первый четырёхсимвольный код (94^3).
TEST(CodeBook, ReserveCoversWholeArena) {
    constexpr std::array<std::uint32_t, 7> kCounts{1, 94, 95, 94 * 94, 94 * 94 + 1, 94 * 94 * 94, 94 * 94 * 94 + 1};
    for (const auto streams : kCounts) {
        Vcd::CodeBook codes;
        codes.reserve(streams);
        const char* const first = codes.assign(SignalId{0}).code.data();
        for (std::uint32_t i = 1; i < streams; ++i)
            std::ignore = codes.assign(SignalId{i});
        EXPECT_EQ(codes.code(0).data(), first) << streams << " потоков";
    }
}
