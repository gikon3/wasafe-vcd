#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <wasafe/io/builder.hpp>
#include <wasafe/io/ingest.hpp>
#include <wasafe/model/database.hpp>

#include "wasafe_vcd/reader.hpp"

/// Общие хелперы тестов VCD: эталонный дамп, временный файл и разбор в память.
namespace VcdTest {

/// Эталонный дамп: скаляр, вектор, real, алиас и вложенный scope.
inline constexpr std::string_view kVcd = R"(
$date Tue $end
$version test $end
$timescale 1ns $end
$scope module top $end
$var wire 1 ! clk $end
$var wire 8 # data[7:0] $end
$var real 64 $ temp $end
$var wire 1 ! clk_mirror $end
$scope module sub $end
$var reg 1 % rst $end
$upscope $end
$upscope $end
$enddefinitions $end
$dumpvars
0!
b00000000 #
r1.5 $
0%
$end
#10
1!
#20
0!
b10100101 #
1%
#30
1!
r2.5 $
)";

/// Временный .vcd файл с автоудалением (вместе с возможным .wsfstore).
struct TempVcd {
    std::filesystem::path path;

    explicit TempVcd(std::string_view name, std::string_view content = kVcd) :
            path(std::filesystem::temp_directory_path() / name) {
        std::ofstream(path, std::ios::binary) << content;
    }
    TempVcd(const TempVcd&) = delete;
    TempVcd(TempVcd&&) = delete;
    ~TempVcd() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(std::filesystem::path(path) += ".wsfstore", ec);
    }

    TempVcd& operator=(const TempVcd&) = delete;
    TempVcd& operator=(TempVcd&&) = delete;
};

/// Разобрать дамп в память одним вызовом.
inline WaSafe::Database parseToMemory(const std::filesystem::path& path) {
    WaSafe::Vcd::VcdReader reader{path};
    auto builder = WaSafe::makeMemoryBuilder();
    return WaSafe::ingest(reader, *builder);
}

/// Общие проверки содержимого разобранного kVcd.
inline void checkContents(const WaSafe::Database& db) {
    EXPECT_EQ(db.timeScale(), (WaSafe::TimeScale{.exponent = -9, .scale = 1}));

    const auto clk = db.find("top.clk");
    ASSERT_TRUE(clk);
    EXPECT_EQ(clk->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(10).asLogic().toString(), "1");
    EXPECT_EQ(clk->valueAt(25).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(35).asLogic().toString(), "1");

    const auto data = db.find("top.data");
    ASSERT_TRUE(data);
    EXPECT_EQ(data->type()->bitWidth(), 8u);
    EXPECT_EQ(data->valueAt(0).asLogic().toString(), "00000000");
    EXPECT_EQ(data->valueAt(25).asLogic().toString(), "10100101");

    const auto temp = db.find("top.temp");
    ASSERT_TRUE(temp);
    EXPECT_EQ(temp->kind(), WaSafe::TypeKind::REAL);
    EXPECT_DOUBLE_EQ(temp->valueAt(0).asReal(), 1.5);
    EXPECT_DOUBLE_EQ(temp->valueAt(35).asReal(), 2.5);

    const auto rst = db.find("top.sub.rst");
    ASSERT_TRUE(rst);
    EXPECT_EQ(rst->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(rst->valueAt(25).asLogic().toString(), "1");

    // Алиас кода '!' делит поток с clk.
    const auto mirror = db.find("top.clk_mirror");
    ASSERT_TRUE(mirror);
    EXPECT_EQ(mirror->valueAt(10).asLogic().toString(), "1");
    EXPECT_EQ(mirror->valueAt(25).asLogic().toString(), "0");
}

}  // namespace VcdTest
