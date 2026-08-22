#include <gtest/gtest.h>

#include <clocale>
#include <filesystem>
#include <string>
#include <string_view>
#include <wasafe/io/builder.hpp>
#include <wasafe/io/ingest.hpp>
#include <wasafe/io/reader.hpp>
#include <wasafe/model/database.hpp>

#include "vcd_test_util.hpp"
#include "wasafe_vcd/exception.hpp"
#include "wasafe_vcd/reader.hpp"

using namespace WaSafe;
using namespace VcdTest;

// VCD: проверка builder через ValidateBuilder
TEST(Vcd, ValidateBuilder) {
    const TempVcd tmp{"wasafe_validate.vcd"};

    Vcd::VcdReader reader{tmp.path};
    auto builder = makeMemoryBuilder();
    auto validateBuilder = makeValidatingBuilder(*builder);
    reader.read(*validateBuilder);
    validateBuilder->finish();
}

// VCD: прямой разбор в MemoryBuilder
TEST(Vcd, DirectParseIntoMemoryBuilder) {
    const TempVcd tmp{"wasafe_direct.vcd"};

    Vcd::VcdReader reader{tmp.path};
    auto builder = makeMemoryBuilder();
    reader.read(*builder);
    builder->finish();

    checkContents(builder->takeDatabase());
}

// VCD: ingest в память (MemoryBuilder)
TEST(Vcd, IngestIntoMemory) {
    const TempVcd tmp{"wasafe_ingest_memory.vcd"};

    Vcd::VcdReader reader{tmp.path};
    auto builder = makeMemoryBuilder();

    checkContents(ingest(reader, *builder));
}

// VCD: ingest в нормализованный store на диске, чтение поверх него — ленивое
TEST(Vcd, IngestIntoLazyStore) {
    const TempVcd tmp{"wasafe_ingest_lazy.vcd"};
    const auto store = std::filesystem::path(tmp.path) += ".wsfstore";

    Vcd::VcdReader reader{tmp.path};
    auto builder = makeIndexingBuilder(store);

    const auto db = ingest(reader, *builder);

    EXPECT_TRUE(std::filesystem::exists(store));
    checkContents(db);
}

// VCD: ingestHeader строит иерархию без значений
TEST(Vcd, IngestHeaderBuildsHierarchyOnly) {
    const TempVcd tmp{"wasafe_header.vcd"};

    Vcd::VcdReader reader{tmp.path};
    auto builder = makeMemoryBuilder();

    const auto db = ingestHeader(reader, *builder);

    // Иерархия есть...
    EXPECT_TRUE(db.find("top.clk"));
    EXPECT_TRUE(db.find("top.sub.rst"));
    // ...а значений нет (заголовок не читал поток изменений).
    EXPECT_FALSE(db.find("top.clk")->valueAt(10).valid());
}

// ---------------------------------------------------------------------------
// Корректность разбора
// ---------------------------------------------------------------------------

// Один и тот же VcdReader должен переиспользоваться: состояние разбора живёт
// внутри вызова, иначе второй проход принял бы все $var за алиасы.
TEST(Vcd, ReaderIsReusableBetweenParses) {
    const TempVcd tmp{"wasafe_reuse.vcd"};

    Vcd::VcdReader reader{tmp.path};

    auto headerBuilder = makeMemoryBuilder();
    const Database header = ingestHeader(reader, *headerBuilder);
    EXPECT_TRUE(header.find("top.clk"));

    auto fullBuilder = makeMemoryBuilder();
    checkContents(ingest(reader, *fullBuilder));
}

// IEEE 1364: значение короче объявленной ширины расширяется влево нулём для
// 0/1 и самим значением для x/z — и для векторной, и для скалярной формы.
TEST(Vcd, ExtendsShortValuesToSignalWidth) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 8 ! a $end
$var wire 8 # b $end
$var wire 8 $ c $end
$var wire 8 % d $end
$upscope $end
$enddefinitions $end
#0
b1 !
bx #
bz $
x%
#10
b101 !
)";
    const TempVcd tmp{"wasafe_extend.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    EXPECT_EQ(db.find("top.a")->valueAt(0).asLogic().toString(), "00000001");
    EXPECT_EQ(db.find("top.a")->valueAt(10).asLogic().toString(), "00000101");
    EXPECT_EQ(db.find("top.b")->valueAt(0).asLogic().toString(), "xxxxxxxx");
    EXPECT_EQ(db.find("top.c")->valueAt(0).asLogic().toString(), "zzzzzzzz");
    EXPECT_EQ(db.find("top.d")->valueAt(0).asLogic().toString(), "xxxxxxxx");
}

// Значение шире сигнала: лишние старшие разряды отбрасываются, а хвост
// последнего слова обязан остаться нулевым.
TEST(Vcd, TruncatesOverlongValues) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 4 ! a $end
$var wire 4 # b $end
$upscope $end
$enddefinitions $end
#0
b110101101 !
bxxxx1010 #
)";
    const TempVcd tmp{"wasafe_truncate.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    EXPECT_EQ(db.find("top.a")->valueAt(0).asLogic().toString(), "1101");
    // Иксы ушли вместе со старшими разрядами — значение снова двухзначное.
    EXPECT_EQ(db.find("top.b")->valueAt(0).asLogic().toString(), "1010");
    EXPECT_TRUE(db.find("top.b")->valueAt(0).asLogic().isTwoState());
}

// Битовый диапазон в ссылке принимается, только если он сходится с размером из
// $var: иначе тип и поток значений разъехались бы по ширине.
TEST(Vcd, PrefersDeclaredSizeOverInconsistentRange) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 8 ! good [15:8] $end
$var wire 8 # bad [3:0] $end
$upscope $end
$enddefinitions $end
#0
b10100101 !
b10100101 #
)";
    const TempVcd tmp{"wasafe_range.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    EXPECT_EQ(db.find("top.good")->type()->bitWidth(), 8u);
    EXPECT_EQ(db.find("top.good")->valueAt(0).asLogic().toString(), "10100101");
    EXPECT_EQ(db.find("top.bad")->type()->bitWidth(), 8u);
    EXPECT_EQ(db.find("top.bad")->valueAt(0).asLogic().toString(), "10100101");
}

// Форма изменения должна сходиться с объявленным типом: 'b' для real и 'r' для
// логики — это мусор, который не должен попадать в поток.
TEST(Vcd, IgnoresValueFormsForeignToDeclaredType) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var real 64 ! temp $end
$var wire 8 # data $end
$upscope $end
$enddefinitions $end
#0
r1.5 !
b10100101 #
#10
b1111 !
r2.5 #
)";
    const TempVcd tmp{"wasafe_forms.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(0).asReal(), 1.5);
    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(10).asReal(), 1.5);  // 'b' проигнорирован
    EXPECT_EQ(db.find("top.data")->valueAt(0).asLogic().toString(), "10100101");
    EXPECT_EQ(db.find("top.data")->valueAt(10).asLogic().toString(), "10100101");  // 'r' проигнорирован
}

TEST(Vcd, ReadsStringVariables) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var string 1 ! state $end
$upscope $end
$enddefinitions $end
#0
sIDLE !
#10
sRUNNING !
)";
    const TempVcd tmp{"wasafe_string.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const auto state = db.find("top.state");
    ASSERT_TRUE(state);
    EXPECT_EQ(state->kind(), TypeKind::STRING);
    EXPECT_EQ(state->valueAt(0).asString(), "IDLE");
    EXPECT_EQ(state->valueAt(15).asString(), "RUNNING");
}

// Событийные переменные дампятся как скаляр '1' — читаются как однобитовый сигнал.
TEST(Vcd, ReadsEventVariables) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var event 1 ! tick $end
$upscope $end
$enddefinitions $end
#10
1!
)";
    const TempVcd tmp{"wasafe_event.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const auto tick = db.find("top.tick");
    ASSERT_TRUE(tick);
    EXPECT_EQ(tick->type()->bitWidth(), 1u);
    EXPECT_EQ(tick->valueAt(10).asLogic().toString(), "1");
}

// Обёртки дампа прозрачны, $comment со словом $end внутри заканчивается на
// первом же $end, а CRLF ничем не отличается от LF.
TEST(Vcd, HandlesDumpSectionsCommentsAndCrLf) {
    const std::string src =
            "$comment nested $end tail $end\r\n"
            "$timescale 1ns $end\r\n"
            "$scope module top $end\r\n"
            "$var wire 1 ! clk $end\r\n"
            "$upscope $end\r\n"
            "$enddefinitions $end\r\n"
            "$dumpvars\r\n0!\r\n$end\r\n"
            "#10\r\n$dumpoff\r\nx!\r\n$end\r\n"
            "#20\r\n$dumpon\r\n1!\r\n$end\r\n"
            "#30\r\nz!\r\n";
    const TempVcd tmp{"wasafe_dump.vcd", src};
    const Database db = parseToMemory(tmp.path);

    EXPECT_EQ(db.timeScale(), (TimeScale{.exponent = -9, .scale = 1}));
    const auto clk = db.find("top.clk");
    ASSERT_TRUE(clk);
    EXPECT_EQ(clk->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(15).asLogic().toString(), "x");
    EXPECT_EQ(clk->valueAt(25).asLogic().toString(), "1");
    EXPECT_EQ(clk->valueAt(35).asLogic().toString(), "z");
    // "tail" из комментария не должен был превратиться в имя сигнала.
    EXPECT_FALSE(db.find("top.tail"));
}

// Раньше $scope без имени съедал $end и вместе с ним следующую директиву.
TEST(Vcd, RejectsScopeWithoutName) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module $end
$var wire 1 ! clk $end
$upscope $end
$enddefinitions $end
)";
    const TempVcd tmp{"wasafe_badscope.vcd", kSrc};

    EXPECT_THROW((void)parseToMemory(tmp.path), Vcd::Error);
}

TEST(Vcd, RejectsBadTimestamp) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! clk $end
$upscope $end
$enddefinitions $end
#abc
1!
)";
    const TempVcd tmp{"wasafe_badtime.vcd", kSrc};

    EXPECT_THROW((void)parseToMemory(tmp.path), Vcd::Error);
}

// Разбор real не должен зависеть от локали: с LC_NUMERIC, где разделитель —
// запятая, strtod прочитал бы "1.5" как 1.
TEST(Vcd, ParsesRealsIndependentlyOfLocale) {
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
)";
    const TempVcd tmp{"wasafe_locale.vcd", kSrc};

    const char* const saved = std::setlocale(LC_NUMERIC, nullptr);
    const std::string previous = saved != nullptr ? saved : "C";
    // Локали с запятой может не быть в системе — тогда проверяем в текущей.
    for (const char* name : {"de_DE.UTF-8", "ru_RU.UTF-8", "C"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr)
            break;
    }

    const Database db = parseToMemory(tmp.path);
    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(0).asReal(), 1.5);
    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(10).asReal(), -225.0);

    std::setlocale(LC_NUMERIC, previous.c_str());
}

// ---------------------------------------------------------------------------
// Одиночные индексы в ссылке: элементы объединяются в шину
// ---------------------------------------------------------------------------

// Бит-бласт: каждый бит отдельным $var — шина собирается массивом скаляров.
TEST(Vcd, MergesBitBlastedSignalsIntoBus) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! data[0] $end
$var wire 1 " data[1] $end
$var wire 1 # data[2] $end
$var wire 1 $ data[3] $end
$upscope $end
$enddefinitions $end
#0
0!
1"
0#
1$
#10
1!
#20
0$
)";
    const TempVcd tmp{"wasafe_bitblast.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const auto bus = db.find("top.data");
    ASSERT_TRUE(bus);
    EXPECT_EQ(bus->kind(), TypeKind::ARRAY);
    EXPECT_EQ(bus->childCount(), 4u);

    // Каждый бит адресуется по пути и хранит собственный поток.
    EXPECT_EQ(db.find("top.data[0]")->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(db.find("top.data[0]")->valueAt(10).asLogic().toString(), "1");
    EXPECT_EQ(db.find("top.data[3]")->valueAt(10).asLogic().toString(), "1");
    EXPECT_EQ(db.find("top.data[3]")->valueAt(20).asLogic().toString(), "0");

    // Значение шины — агрегат из битов в порядке измерения [0:3].
    const Value agg = bus->valueAt(0);
    ASSERT_TRUE(agg.isAggregate());
    ASSERT_EQ(agg.asAggregate().size(), 4u);
    EXPECT_EQ(agg.asAggregate()[0].asLogic().toString(), "0");  // data[0]
    EXPECT_EQ(agg.asAggregate()[1].asLogic().toString(), "1");  // data[1]

    // changes по шине сливает потоки всех битов.
    std::vector<TimeStamp> seen;
    auto cur = bus->changes({0, 30});
    while (cur.next())
        seen.push_back(cur->time);
    EXPECT_EQ(seen, (std::vector<TimeStamp>{0, 0, 0, 0, 10, 20}));
}

// Элементы шире бита: массив векторов, ширина берётся у ЛИСТА, а не у массива.
TEST(Vcd, MergesWideElementsKeepingLeafWidth) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 8 ! mem[0] $end
$var wire 8 " mem[1] $end
$var wire 8 # mem[2] $end
$upscope $end
$enddefinitions $end
#0
b10100101 !
b00001111 "
b11110000 #
)";
    const TempVcd tmp{"wasafe_mem.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const auto mem = db.find("top.mem");
    ASSERT_TRUE(mem);
    EXPECT_EQ(mem->kind(), TypeKind::ARRAY);
    EXPECT_EQ(mem->childCount(), 3u);

    const auto e0 = db.find("top.mem[0]");
    ASSERT_TRUE(e0);
    EXPECT_EQ(e0->kind(), TypeKind::VECTOR);
    EXPECT_EQ(e0->width(), 8u);  // ширина листа, а не нулевая ширина массива
    EXPECT_EQ(e0->valueAt(0).asLogic().toString(), "10100101");
    EXPECT_EQ(db.find("top.mem[2]")->valueAt(0).asLogic().toString(), "11110000");
}

// Вложенность: индексы любой глубины дают вложенные массивы.
TEST(Vcd, MergesNestedIndicesIntoNestedArrays) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 4 ! m[0][0] $end
$var wire 4 " m[0][1] $end
$var wire 4 # m[1][0] $end
$var wire 4 $ m[1][1] $end
$upscope $end
$enddefinitions $end
#0
b0001 !
b0010 "
b0100 #
b1000 $
)";
    const TempVcd tmp{"wasafe_nested.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const auto m = db.find("top.m");
    ASSERT_TRUE(m);
    EXPECT_EQ(m->kind(), TypeKind::ARRAY);
    EXPECT_EQ(m->childCount(), 2u);  // внешнее измерение
    EXPECT_EQ(m->child(0).kind(), TypeKind::ARRAY);
    EXPECT_EQ(m->child(0).childCount(), 2u);

    EXPECT_EQ(db.find("top.m[0][0]")->valueAt(0).asLogic().toString(), "0001");
    EXPECT_EQ(db.find("top.m[0][1]")->valueAt(0).asLogic().toString(), "0010");
    EXPECT_EQ(db.find("top.m[1][0]")->valueAt(0).asLogic().toString(), "0100");
    EXPECT_EQ(db.find("top.m[1][1]")->valueAt(0).asLogic().toString(), "1000");
}

// Индекс + диапазон: packed-часть остаётся вектором, массив — unpacked.
TEST(Vcd, MergesIndexedVectorsAsUnpackedArrayOfPackedElements) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 8 ! mem[0][7:0] $end
$var wire 8 " mem[1][7:0] $end
$upscope $end
$enddefinitions $end
#0
b10100101 !
b01011010 "
)";
    const TempVcd tmp{"wasafe_idx_range.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const auto mem = db.find("top.mem");
    ASSERT_TRUE(mem);
    const auto* arr = mem->type()->as<ArrayType>();
    ASSERT_NE(arr, nullptr);
    EXPECT_FALSE(arr->packed());  // поток на элемент, а не битовые срезы одного
    EXPECT_EQ(arr->elementType()->bitWidth(), 8u);

    EXPECT_EQ(db.find("top.mem[0]")->valueAt(0).asLogic().toString(), "10100101");
    EXPECT_EQ(db.find("top.mem[1]")->valueAt(0).asLogic().toString(), "01011010");
}

// VCD не хранит направление измерения — оно берётся из порядка объявления.
TEST(Vcd, DimensionDirectionFollowsDeclarationOrder) {
    constexpr std::string_view kAscending = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! s[0] $end
$var wire 1 " s[1] $end
$var wire 1 # s[2] $end
$upscope $end
$enddefinitions $end
#0
1!
0"
0#
)";
    constexpr std::string_view kDescending = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! s[2] $end
$var wire 1 " s[1] $end
$var wire 1 # s[0] $end
$upscope $end
$enddefinitions $end
#0
1!
0"
0#
)";
    {
        const TempVcd tmp{"wasafe_dir_asc.vcd", kAscending};
        const Database db = parseToMemory(tmp.path);
        const auto* arr = db.find("top.s")->type()->as<ArrayType>();
        ASSERT_NE(arr, nullptr);
        EXPECT_EQ(arr->indexLeft(), 0);
        EXPECT_EQ(arr->indexRight(), 2);
        // Порядок агрегата следует направлению: первый элемент — s[0].
        EXPECT_EQ(db.find("top.s")->valueAt(0).asAggregate()[0].asLogic().toString(), "1");
    }
    {
        const TempVcd tmp{"wasafe_dir_desc.vcd", kDescending};
        const Database db = parseToMemory(tmp.path);
        const auto* arr = db.find("top.s")->type()->as<ArrayType>();
        ASSERT_NE(arr, nullptr);
        EXPECT_EQ(arr->indexLeft(), 2);
        EXPECT_EQ(arr->indexRight(), 0);
        // Первый элемент агрегата — s[2], он же объявлен первым.
        EXPECT_EQ(db.find("top.s")->valueAt(0).asAggregate()[0].asLogic().toString(), "1");
        EXPECT_EQ(db.find("top.s[2]")->valueAt(0).asLogic().toString(), "1");
    }
}

// Индексы бывают отрицательными — знак разбирается наравне с остальным.
TEST(Vcd, MergesNegativeIndices) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! sig[-1] $end
$var wire 1 " sig[0] $end
$var wire 1 # sig[1] $end
$upscope $end
$enddefinitions $end
#0
1!
0"
1#
)";
    const TempVcd tmp{"wasafe_negidx.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const auto* arr = db.find("top.sig")->type()->as<ArrayType>();
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->indexLeft(), -1);
    EXPECT_EQ(arr->indexRight(), 1);
    EXPECT_EQ(db.find("top.sig[-1]")->valueAt(0).asLogic().toString(), "1");
    EXPECT_EQ(db.find("top.sig[1]")->valueAt(0).asLogic().toString(), "1");
}

// Код, общий у обычного сигнала и у элемента шины, — алиас на уровне элемента:
// данные видны обоим, в каком бы порядке $var ни шли.
TEST(Vcd, SharedCodeAliasesBusElement) {
    constexpr std::string_view kPlainFirst = R"(
$timescale 1ns $end
$scope module top $end
$var wire 4 ! probe $end
$var wire 4 ! mem[0] $end
$var wire 4 " mem[1] $end
$upscope $end
$enddefinitions $end
#0
b1011 !
b0100 "
)";
    constexpr std::string_view kElementFirst = R"(
$timescale 1ns $end
$scope module top $end
$var wire 4 ! mem[0] $end
$var wire 4 " mem[1] $end
$var wire 4 ! probe $end
$upscope $end
$enddefinitions $end
#0
b1011 !
b0100 "
)";
    for (const auto src : {kPlainFirst, kElementFirst}) {
        const TempVcd tmp{"wasafe_alias_bus.vcd", src};
        const Database db = parseToMemory(tmp.path);

        ASSERT_TRUE(db.find("top.mem")) << src;
        EXPECT_EQ(db.find("top.probe")->valueAt(0).asLogic().toString(), "1011") << src;
        EXPECT_EQ(db.find("top.mem[0]")->valueAt(0).asLogic().toString(), "1011") << src;
        EXPECT_EQ(db.find("top.mem[1]")->valueAt(0).asLogic().toString(), "0100") << src;
    }
}

// Базовое имя, занятое обычным сигналом, шину не образует: иначе в scope
// оказались бы два сигнала с одним именем.
TEST(Vcd, KeepsLiteralNamesWhenBaseNameTaken) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 8 ! data[7:0] $end
$var wire 1 " data[4][1] $end
$upscope $end
$enddefinitions $end
#0
b10100101 !
1"
)";
    const TempVcd tmp{"wasafe_name_clash.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    // Обычный сигнал не тронут.
    const auto data = db.find("top.data");
    ASSERT_TRUE(data);
    EXPECT_EQ(data->kind(), TypeKind::VECTOR);
    EXPECT_EQ(data->width(), 8u);
    EXPECT_EQ(data->valueAt(0).asLogic().toString(), "10100101");

    // Индексированный остался под литеральным именем и потому недостижим через
    // find (путь разбирается как data + [4] + [1]) — только обходом scope.
    EXPECT_FALSE(db.find("top.data[4][1]"));
    const Scope top = db.root().scope("top");
    EXPECT_EQ(top.signalCount(), 2u);
    const Signal literal = top.signal(1);
    EXPECT_EQ(literal.name(), "data[4][1]");
    EXPECT_EQ(literal.valueAt(0).asLogic().toString(), "1");
}

// Неоднородная группа шиной не становится: собрать массив из элементов разной
// размерности или разной ширины нечем.
TEST(Vcd, KeepsLiteralNamesForInconsistentGroups) {
    constexpr std::string_view kMixedRank = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! a[0] $end
$var wire 1 " a[1][2] $end
$upscope $end
$enddefinitions $end
#0
1!
0"
)";
    constexpr std::string_view kMixedWidth = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! b[0] $end
$var wire 8 " b[1] $end
$upscope $end
$enddefinitions $end
#0
1!
b10100101 "
)";
    {
        const TempVcd tmp{"wasafe_mixed_rank.vcd", kMixedRank};
        const Database db = parseToMemory(tmp.path);
        EXPECT_FALSE(db.find("top.a"));
        const Scope top = db.root().scope("top");
        ASSERT_EQ(top.signalCount(), 2u);
        EXPECT_EQ(top.signal(0).name(), "a[0]");
        EXPECT_EQ(top.signal(0).valueAt(0).asLogic().toString(), "1");
        EXPECT_EQ(top.signal(1).name(), "a[1][2]");
    }
    {
        const TempVcd tmp{"wasafe_mixed_width.vcd", kMixedWidth};
        const Database db = parseToMemory(tmp.path);
        EXPECT_FALSE(db.find("top.b"));
        const Scope top = db.root().scope("top");
        ASSERT_EQ(top.signalCount(), 2u);
        EXPECT_EQ(top.signal(1).name(), "b[1]");
        EXPECT_EQ(top.signal(1).valueAt(0).asLogic().toString(), "10100101");
    }
}

// Один ещё не зарегистрированный код на двух элементах: привязать их к общему
// потоку одним declareVar нечем, поэтому группа остаётся поэлементной.
TEST(Vcd, KeepsLiteralNamesOnDuplicateCodeInsideGroup) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 4 ! dup[0] $end
$var wire 4 ! dup[1] $end
$upscope $end
$enddefinitions $end
#0
b1011 !
)";
    const TempVcd tmp{"wasafe_dup_code.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    EXPECT_FALSE(db.find("top.dup"));
    const Scope top = db.root().scope("top");
    ASSERT_EQ(top.signalCount(), 2u);
    // Оба — алиасы одного потока, данные не потеряны ни у одного.
    EXPECT_EQ(top.signal(0).valueAt(0).asLogic().toString(), "1011");
    EXPECT_EQ(top.signal(1).valueAt(0).asLogic().toString(), "1011");
}

// Разрежённый набор индексов не должен материализовать гору пустых потоков.
TEST(Vcd, DoesNotMergeSparseIndexSets) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! sparse[0] $end
$var wire 1 " sparse[100000] $end
$upscope $end
$enddefinitions $end
#0
1!
0"
)";
    const TempVcd tmp{"wasafe_sparse.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    EXPECT_FALSE(db.find("top.sparse"));
    EXPECT_EQ(db.root().scope("top").signalCount(), 2u);
}

// Шина занимает место своего первого элемента: порядок объявления сохраняется.
TEST(Vcd, PreservesDeclarationOrderAcrossBuses) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! clk $end
$var wire 1 " bus[0] $end
$var wire 1 # other $end
$var wire 1 $ bus[1] $end
$upscope $end
$enddefinitions $end
#0
0!
1"
0#
1$
)";
    const TempVcd tmp{"wasafe_order.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    const Scope top = db.root().scope("top");
    ASSERT_EQ(top.signalCount(), 3u);
    EXPECT_EQ(top.signal(0).name(), "clk");
    EXPECT_EQ(top.signal(1).name(), "bus");
    EXPECT_EQ(top.signal(2).name(), "other");
    EXPECT_EQ(db.find("top.bus[1]")->valueAt(0).asLogic().toString(), "1");
}

// Шины собираются в своём scope: одинаковые базовые имена на разных уровнях
// не смешиваются, а незакрытый $scope не роняет разбор.
TEST(Vcd, GroupsAreScopeLocal) {
    constexpr std::string_view kSrc = R"(
$timescale 1ns $end
$scope module top $end
$var wire 1 ! bus[0] $end
$scope module sub $end
$var wire 1 " bus[0] $end
$var wire 1 # bus[1] $end
$enddefinitions $end
#0
1!
0"
1#
)";
    const TempVcd tmp{"wasafe_scoped.vcd", kSrc};
    const Database db = parseToMemory(tmp.path);

    EXPECT_EQ(db.find("top.bus[0]")->valueAt(0).asLogic().toString(), "1");
    EXPECT_EQ(db.find("top.sub.bus[0]")->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(db.find("top.sub.bus[1]")->valueAt(0).asLogic().toString(), "1");
    EXPECT_EQ(db.find("top.sub.bus")->childCount(), 2u);
}
