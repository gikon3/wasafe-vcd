#include <gtest/gtest.h>

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <wasafe/model/database.hpp>
#include <wasafe/model/scope.hpp>
#include <wasafe/types/type.hpp>

#include "vcd_syntax.hpp"
#include "vcd_test_util.hpp"
#include "wasafe_vcd/version.hpp"
#include "wasafe_vcd/writer.hpp"

using namespace WaSafe;
using namespace VcdTest;

namespace {

/// Тип event: фабрики для него нет, а лексике нужен только вид.
class EventType final : public TypeDescriptor {
public:
    EventType() : TypeDescriptor{TypeKind::EVENT, false} {}

    [[nodiscard]] std::uint32_t bitWidth() const noexcept override { return 0; }
};

/// Ссылка из последовательности локальных имён.
std::string reference(std::initializer_list<std::string_view> parts) {
    std::string ref;
    for (const auto part : parts)
        Vcd::appendReferencePart(ref, part);
    return ref;
}

/// Только диапазон, дописанный к пустой ссылке.
std::string range(const Type& t) {
    std::string ref;
    Vcd::appendRange(ref, *t);
    return ref;
}

/// Перечисление с одной константой поверх заданной базы.
Type enumOver(Type base) {
    return std::make_shared<const EnumType>(std::move(base), std::vector<EnumEntry>{{"A", 0}});
}

}  // namespace

// ---------------------------------------------------------------------------
// scopeKeyword
// ---------------------------------------------------------------------------

TEST(VcdSyntaxWrite, ScopeKeywordInvertsScopeKind) {
    for (const auto k : {ScopeKind::MODULE, ScopeKind::TASK, ScopeKind::FUNCTION, ScopeKind::BLOCK})
        EXPECT_EQ(Vcd::scopeKind(Vcd::scopeKeyword(k)), k) << Vcd::scopeKeyword(k);
}

TEST(VcdSyntaxWrite, ScopeKeywordFallsBackToModule) {
    for (const auto k :
            {ScopeKind::ROOT, ScopeKind::INTERFACE, ScopeKind::PACKAGE, ScopeKind::PROGRAM, ScopeKind::GENERATE_BLOCK,
                    ScopeKind::STRUCT, ScopeKind::UNION, ScopeKind::ARRAY, ScopeKind::CLASS, ScopeKind::UNKNOWN}) {
        EXPECT_EQ(Vcd::scopeKeyword(k), "module") << static_cast<int>(k);
    }
}

// ---------------------------------------------------------------------------
// varKeyword / varSize
// ---------------------------------------------------------------------------

TEST(VcdSyntaxWrite, DescribesBitTypesAsWire) {
    const Type scalar = makeScalar();
    const Type down = makeVector(7, 0);
    const Type up = makeVector(0, 7);
    const Type enumeration = std::make_shared<const EnumType>(makeVector(8, 1), std::vector<EnumEntry>{{"A", 0}});
    const Type packedArray = makeArray(makeVector(3, 0), 0, 2, true);
    const Type packedStruct = makeStruct({{"a", makeVector(3, 0), 1}, {"b", makeScalar(), 0}}, true);

    for (const auto& t : {scalar, down, up, enumeration, packedArray, packedStruct})
        EXPECT_EQ(Vcd::varKeyword(*t), "wire") << toSvString(t);

    EXPECT_EQ(Vcd::varSize(*scalar), 1U);
    EXPECT_EQ(Vcd::varSize(*down), 8U);
    EXPECT_EQ(Vcd::varSize(*up), 8U);
    EXPECT_EQ(Vcd::varSize(*enumeration), 8U);
    EXPECT_EQ(Vcd::varSize(*packedArray), 12U);
    EXPECT_EQ(Vcd::varSize(*packedStruct), 5U);
}

TEST(VcdSyntaxWrite, DescribesNonBitTypes) {
    const Type real = makeReal();
    const Type shortreal = makeReal(true);
    const Type string = makeString();
    const EventType event;

    EXPECT_EQ(Vcd::varKeyword(*real), "real");
    EXPECT_EQ(Vcd::varSize(*real), 64U);
    EXPECT_EQ(Vcd::varKeyword(*shortreal), "shortreal");
    EXPECT_EQ(Vcd::varSize(*shortreal), 32U);
    EXPECT_EQ(Vcd::varKeyword(*string), "string");
    EXPECT_EQ(Vcd::varSize(*string), 1U);
    EXPECT_EQ(Vcd::varKeyword(event), "event");
    EXPECT_EQ(Vcd::varSize(event), 1U);
}

TEST(VcdSyntaxWrite, VarKeywordReadsBackAsSameValueKind) {
    EXPECT_EQ(Vcd::varType(Vcd::varKeyword(*makeScalar())), Vcd::VarType::LOGIC);
    EXPECT_EQ(Vcd::varType(Vcd::varKeyword(*makeVector(7, 0))), Vcd::VarType::LOGIC);
    EXPECT_EQ(Vcd::varType(Vcd::varKeyword(EventType{})), Vcd::VarType::LOGIC);
    EXPECT_EQ(Vcd::varType(Vcd::varKeyword(*makeReal())), Vcd::VarType::REAL);
    EXPECT_EQ(Vcd::varType(Vcd::varKeyword(*makeReal(true))), Vcd::VarType::SHORT_REAL);
    EXPECT_EQ(Vcd::varType(Vcd::varKeyword(*makeString())), Vcd::VarType::STRING);
}

// ---------------------------------------------------------------------------
// appendReferencePart
// ---------------------------------------------------------------------------

TEST(VcdSyntaxWrite, JoinsReferenceParts) {
    EXPECT_EQ(reference({"data"}), "data");
    EXPECT_EQ(reference({"mem", "[3]"}), "mem[3]");
    EXPECT_EQ(reference({"mem", "[3]", "[0]"}), "mem[3][0]");
    EXPECT_EQ(reference({"s", "addr"}), "s.addr");
    EXPECT_EQ(reference({"bus", "[2]", "payload", "[1]"}), "bus[2].payload[1]");
}

TEST(VcdSyntaxWrite, IgnoresEmptyReferencePart) {
    EXPECT_EQ(reference({"", "data"}), "data");
    EXPECT_EQ(reference({"s", "", "addr"}), "s.addr");
}

TEST(VcdSyntaxWrite, StartsReferenceWithIndexAsIs) {
    EXPECT_EQ(reference({"[0]"}), "[0]");
    EXPECT_EQ(reference({"[0]", "x"}), "[0].x");
}

// ---------------------------------------------------------------------------
// appendRange
// ---------------------------------------------------------------------------

TEST(VcdSyntaxWrite, AppendsVectorRange) {
    EXPECT_EQ(range(makeVector(7, 0)), " [7:0]");
    EXPECT_EQ(range(makeVector(0, 7)), " [0:7]");
    EXPECT_EQ(range(makeVector(-1, -8)), " [-1:-8]");
    EXPECT_EQ(range(makeVector(40, 33)), " [40:33]");

    std::string ref = "bus";
    Vcd::appendRange(ref, *makeVector(3, 0));
    EXPECT_EQ(ref, "bus [3:0]");
}

TEST(VcdSyntaxWrite, AppendsExtremeBounds) {
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();

    EXPECT_EQ(range(makeVector(kMin, kMin + 1)), " [-2147483648:-2147483647]");
    EXPECT_EQ(range(makeVector(kMax, kMax - 1)), " [2147483647:2147483646]");
}

TEST(VcdSyntaxWrite, OmitsRangeWithoutVectorBounds) {
    EXPECT_EQ(range(makeScalar()), "");
    EXPECT_EQ(range(makeVector(5, 5)), "");
    EXPECT_EQ(range(makeReal()), "");
    EXPECT_EQ(range(makeString()), "");
    EXPECT_EQ(range(makeArray(makeVector(3, 0), 0, 2, true)), "");
    EXPECT_EQ(range(makeStruct({{"a", makeVector(3, 0), 1}, {"b", makeScalar(), 0}}, true)), "");
}

TEST(VcdSyntaxWrite, EnumRangeComesFromVectorBase) {
    EXPECT_EQ(range(enumOver(makeVector(8, 1))), " [8:1]");
    EXPECT_EQ(range(enumOver(makeVector(0, 7))), " [0:7]");

    EXPECT_EQ(range(enumOver(makeVector(5, 5))), "");
    EXPECT_EQ(range(enumOver(makeScalar())), "");
    EXPECT_EQ(range(enumOver(makeStruct({{"a", makeVector(3, 0), 1}, {"b", makeScalar(), 0}}, true))), "");
    EXPECT_EQ(range(enumOver(nullptr)), "");
}

TEST(VcdSyntaxWrite, RangeReadsBackThroughParseReference) {
    // Разбор склеивает токены ссылки без пробела.
    const auto parse = [](std::string ref, std::vector<std::int32_t>& indices) {
        std::erase(ref, ' ');
        const Vcd::Reference r = Vcd::parseReference(ref, indices);
        return std::tuple{std::string{r.name}, r.msb, r.lsb, r.hasRange};
    };
    std::vector<std::int32_t> indices;

    std::string ref = reference({"bus"});
    Vcd::appendRange(ref, *makeVector(-1, -8));
    EXPECT_EQ(parse(ref, indices), std::tuple(std::string{"bus"}, -1, -8, true));
    EXPECT_TRUE(indices.empty());

    ref = reference({"mem", "[3]", "[1]"});
    Vcd::appendRange(ref, *makeVector(0, 7));
    EXPECT_EQ(parse(ref, indices), std::tuple(std::string{"mem"}, 0, 7, true));
    EXPECT_EQ(indices, (std::vector<std::int32_t>{3, 1}));
}

// ---------------------------------------------------------------------------
// VcdWriter: сквозные тесты
// ---------------------------------------------------------------------------

namespace {

/// Дамп, прочитанный и выгруженный обратно: текст выгрузки и её повторный разбор.
struct RoundTrip {
    std::string text;
    Database db;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// src -> разбор в память -> VcdWriter -> текст и повторный разбор.
RoundTrip roundTrip(const std::string& name, std::string_view src) {
    const TempVcd in{name + ".vcd", src};
    const TempVcd out{name + "_out.vcd", ""};
    Vcd::VcdWriter{}.write(parseToMemory(in.path), out.path);
    return {readFile(out.path), parseToMemory(out.path)};
}

/// Сколько строк текста равны line.
std::ptrdiff_t countLines(std::string_view text, std::string_view line) {
    std::ptrdiff_t n = 0;
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t eol = std::min(text.find('\n', pos), text.size());
        n += text.substr(pos, eol - pos) == line ? 1 : 0;
        pos = eol + 1;
    }
    return n;
}

std::string logicAt(const Database& db, std::string_view path, TimeStamp t) {
    const auto sig = db.find(path);
    return sig ? sig->valueAt(t).asLogic().toString() : "<no " + std::string{path} + ">";
}

}  // namespace

// Иерархия и значения переживают выгрузку, а повторная выгрузка прочитанного
// даёт тот же текст: вывод детерминирован.
TEST(VcdWriter, RoundTripsHierarchyAndValues) {
    const RoundTrip first = roundTrip("wasafe_w_roundtrip", kVcd);
    checkContents(first.db);

    const Scope top = first.db.root().scope("top");
    ASSERT_TRUE(top);
    EXPECT_EQ(top.kind(), ScopeKind::MODULE);
    ASSERT_EQ(top.signalCount(), 4u);
    EXPECT_EQ(top.signal(0).name(), "clk");
    EXPECT_EQ(top.signal(1).name(), "data");
    EXPECT_EQ(top.signal(2).name(), "temp");
    EXPECT_EQ(top.signal(3).name(), "clk_mirror");
    ASSERT_EQ(top.scopeCount(), 1u);
    EXPECT_EQ(top.scope(0).name(), "sub");

    const RoundTrip second = roundTrip("wasafe_w_roundtrip2", first.text);
    EXPECT_EQ(second.text, first.text);
}

// Виды scope со своим словом в VCD сохраняются, а сигналы корня пишутся до
// первого $scope и остаются в корне.
TEST(VcdWriter, RoundTripsScopeKindsAndRootSignals) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$var wire 1 ! glob $end
$scope module top $end
$scope task t $end
$var wire 1 " a $end
$upscope $end
$scope function f $end
$var wire 1 # b $end
$upscope $end
$scope begin blk $end
$var wire 1 $ c $end
$upscope $end
$upscope $end
$enddefinitions $end
#0
1!
0"
1#
0$
)";
    const RoundTrip rt = roundTrip("wasafe_w_scopes", kSrc);

    EXPECT_EQ(rt.text.find("$var wire 1 ! glob $end\n"), rt.text.find("$var"));
    EXPECT_LT(rt.text.find("glob"), rt.text.find("$scope"));

    EXPECT_EQ(logicAt(rt.db, "glob", 0), "1");
    const Scope top = rt.db.root().scope("top");
    ASSERT_TRUE(top);
    EXPECT_EQ(top.scope("t").kind(), ScopeKind::TASK);
    EXPECT_EQ(top.scope("f").kind(), ScopeKind::FUNCTION);
    EXPECT_EQ(top.scope("blk").kind(), ScopeKind::BLOCK);
    EXPECT_EQ(logicAt(rt.db, "top.t.a", 0), "0");
    EXPECT_EQ(logicAt(rt.db, "top.f.b", 0), "1");
    EXPECT_EQ(logicAt(rt.db, "top.blk.c", 0), "0");
}

// Все четыре формы записи, диапазоны векторов с нестандартными границами и
// заголовок — побайтово.
TEST(VcdWriter, WritesScalarsVectorsRealsAndStrings) {
    constexpr std::string_view kSrc = R"(
$timescale 10ps $end
$scope module top $end
$var wire 1 ! clk $end
$var wire 8 " bus [8:1] $end
$var wire 4 # asc [0:3] $end
$var real 64 $ temp $end
$var string 1 % state $end
$var event 1 & tick $end
$upscope $end
$enddefinitions $end
#0
x!
b1z0x0101 "
bzzzz #
r0.1 $
sIDLE %
#5
1!
r-2.25e2 $
sRUN %
1&
)";
    const RoundTrip rt = roundTrip("wasafe_w_forms", kSrc);

    const std::string expected = "$version wasafe-vcd " + std::string{Vcd::Version::kString} +
            " $end\n"
            "$timescale 10 ps $end\n"
            "$scope module top $end\n"
            "$var wire 1 ! clk $end\n"
            "$var wire 8 \" bus [8:1] $end\n"
            "$var wire 4 # asc [0:3] $end\n"
            "$var real 64 $ temp $end\n"
            "$var string 1 % state $end\n"
            "$var wire 1 & tick $end\n"
            "$upscope $end\n"
            "$enddefinitions $end\n"
            "#0\n"
            "x!\n"
            "b1z0x0101 \"\n"
            "bz #\n"
            "r0.1 $\n"
            "sIDLE %\n"
            "#5\n"
            "1!\n"
            "r-225 $\n"
            "sRUN %\n"
            "1&\n";
    EXPECT_EQ(rt.text, expected);

    EXPECT_EQ(rt.db.timeScale(), (TimeScale{.exponent = -12, .scale = 10}));
    EXPECT_EQ(logicAt(rt.db, "top.clk", 0), "x");
    EXPECT_EQ(logicAt(rt.db, "top.clk", 5), "1");
    EXPECT_EQ(logicAt(rt.db, "top.bus", 0), "1z0x0101");
    EXPECT_EQ(logicAt(rt.db, "top.asc", 0), "zzzz");
    EXPECT_EQ(logicAt(rt.db, "top.tick", 5), "1");
    EXPECT_DOUBLE_EQ(rt.db.find("top.temp")->valueAt(0).asReal(), 0.1);
    EXPECT_DOUBLE_EQ(rt.db.find("top.temp")->valueAt(5).asReal(), -225.0);
    EXPECT_EQ(rt.db.find("top.state")->valueAt(0).asString(), "IDLE");
    EXPECT_EQ(rt.db.find("top.state")->valueAt(5).asString(), "RUN");

    const auto* bus = rt.db.find("top.bus")->type()->as<VectorType>();
    ASSERT_NE(bus, nullptr);
    EXPECT_EQ(bus->msb(), 8);
    EXPECT_EQ(bus->lsb(), 1);
    const auto* asc = rt.db.find("top.asc")->type()->as<VectorType>();
    ASSERT_NE(asc, nullptr);
    EXPECT_EQ(asc->msb(), 0);
    EXPECT_EQ(asc->lsb(), 3);
}

// Алиасы получают свой $var с общим кодом, а изменения потока пишутся один раз.
TEST(VcdWriter, SharesOneCodeBetweenAliases) {
    const RoundTrip rt = roundTrip("wasafe_w_alias", kVcd);

    EXPECT_EQ(countLines(rt.text, "$var wire 1 ! clk $end"), 1);
    EXPECT_EQ(countLines(rt.text, "$var wire 1 ! clk_mirror $end"), 1);
    EXPECT_EQ(countLines(rt.text, "0!") + countLines(rt.text, "1!"), 4);

    EXPECT_EQ(logicAt(rt.db, "top.clk_mirror", 10), "1");
    EXPECT_EQ(logicAt(rt.db, "top.clk_mirror", 25), "0");
}

// Массивы разворачиваются в $var с индексированными именами, а повторное
// чтение собирает их обратно в массивы.
TEST(VcdWriter, FlattensArraysIntoIndexedNames) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 8 ! mem[0] $end
$var wire 8 " mem[1] $end
$var wire 1 # bits[0] $end
$var wire 1 $ bits[1] $end
$var wire 4 % m[0][0] $end
$var wire 4 & m[0][1] $end
$var wire 4 ' m[1][0] $end
$var wire 4 ( m[1][1] $end
$upscope $end
$enddefinitions $end
#0
b10100101 !
b00001111 "
1#
0$
b0001 %
b0010 &
b0100 '
b1000 (
)";
    const RoundTrip rt = roundTrip("wasafe_w_arrays", kSrc);

    EXPECT_EQ(countLines(rt.text, "$var wire 8 ! mem[0] [7:0] $end"), 1);
    EXPECT_EQ(countLines(rt.text, "$var wire 8 \" mem[1] [7:0] $end"), 1);
    EXPECT_EQ(countLines(rt.text, "$var wire 1 # bits[0] $end"), 1);
    EXPECT_EQ(countLines(rt.text, "$var wire 4 ( m[1][1] [3:0] $end"), 1);

    const auto mem = rt.db.find("top.mem");
    ASSERT_TRUE(mem);
    EXPECT_EQ(mem->kind(), TypeKind::ARRAY);
    EXPECT_EQ(mem->childCount(), 2u);
    EXPECT_EQ(logicAt(rt.db, "top.mem[0]", 0), "10100101");
    EXPECT_EQ(logicAt(rt.db, "top.mem[1]", 0), "00001111");

    const auto bits = rt.db.find("top.bits");
    ASSERT_TRUE(bits);
    EXPECT_EQ(bits->kind(), TypeKind::ARRAY);
    EXPECT_EQ(logicAt(rt.db, "top.bits[0]", 0), "1");

    const auto m = rt.db.find("top.m");
    ASSERT_TRUE(m);
    EXPECT_EQ(m->childCount(), 2u);
    EXPECT_EQ(m->child(0).kind(), TypeKind::ARRAY);
    EXPECT_EQ(logicAt(rt.db, "top.m[0][1]", 0), "0010");
    EXPECT_EQ(logicAt(rt.db, "top.m[1][0]", 0), "0100");
}

// Ведущие нули перед единицей отбрасываются целиком, перед x/z один ноль
// остаётся, ведущие x/z схлопываются в один символ, ведущая единица не трогается.
TEST(VcdWriter, TrimsLeadingRunsButKeepsOnes) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 8 ! a $end
$var wire 8 " b $end
$var wire 8 # c $end
$var wire 8 $ d $end
$var wire 8 % e $end
$var wire 8 & f $end
$var wire 8 ' g $end
$var wire 8 ( h $end
$upscope $end
$enddefinitions $end
#0
b00000001 !
b00x10000 "
b000z0000 #
bxx001100 $
bzzz10000 %
b10000000 &
b00000000 '
bxxxxxxxx (
)";
    const RoundTrip rt = roundTrip("wasafe_w_trim", kSrc);

    const std::vector<std::tuple<std::string_view, std::string_view, std::string_view>> cases{
            {"a", "b1 !", "00000001"},
            {"b", "b0x10000 \"", "00x10000"},
            {"c", "b0z0000 #", "000z0000"},
            {"d", "bx001100 $", "xx001100"},
            {"e", "bz10000 %", "zzz10000"},
            {"f", "b10000000 &", "10000000"},
            {"g", "b0 '", "00000000"},
            {"h", "bx (", "xxxxxxxx"},
    };
    for (const auto& [name, line, value] : cases) {
        EXPECT_EQ(countLines(rt.text, line), 1) << name;
        EXPECT_EQ(logicAt(rt.db, "top." + std::string{name}, 0), value) << name;
    }
}

// Числа пишутся через to_chars: локаль с запятой не должна менять вывод, а
// кратчайшая форма читается обратно в то же число.
TEST(VcdWriter, WritesRealsIndependentlyOfLocale) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var real 64 ! temp $end
$upscope $end
$enddefinitions $end
#0
r1.5 !
#10
r-2.25e2 !
#20
r0.1 !
#30
r1e-300 !
)";
    const char* const saved = std::setlocale(LC_NUMERIC, nullptr);
    const std::string previous = saved != nullptr ? saved : "C";
    // Локали с запятой может не быть в системе — тогда проверяем в текущей.
    for (const char* name : {"de_DE.UTF-8", "ru_RU.UTF-8", "C"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr)
            break;
    }

    const RoundTrip rt = roundTrip("wasafe_w_locale", kSrc);

    std::ignore = std::setlocale(LC_NUMERIC, previous.c_str());

    EXPECT_EQ(countLines(rt.text, "r1.5 !"), 1);
    EXPECT_EQ(countLines(rt.text, "r-225 !"), 1);
    EXPECT_EQ(countLines(rt.text, "r0.1 !"), 1);
    EXPECT_EQ(countLines(rt.text, "r1e-300 !"), 1);

    const auto temp = rt.db.find("top.temp");
    ASSERT_TRUE(temp);
    EXPECT_EQ(temp->valueAt(0).asReal(), 1.5);
    EXPECT_EQ(temp->valueAt(10).asReal(), -225.0);
    EXPECT_EQ(temp->valueAt(20).asReal(), 0.1);
    EXPECT_EQ(temp->valueAt(30).asReal(), 1e-300);
}
