#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "scanner.hpp"
#include "wasafe_vcd/exception.hpp"

using namespace WaSafe;

namespace {

/// Временный файл с произвольным содержимым.
struct TempText {
    std::filesystem::path path;

    TempText(std::string_view name, std::string_view content) : path(std::filesystem::temp_directory_path() / name) {
        std::ofstream(path, std::ios::binary) << content;
    }
    TempText(const TempText&) = delete;
    TempText(TempText&&) = delete;
    ~TempText() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempText& operator=(const TempText&) = delete;
    TempText& operator=(TempText&&) = delete;
};

/// Разобрать весь файл в вектор токенов буфером заданного размера.
std::vector<std::string> tokenize(const std::filesystem::path& path, std::size_t bufferSize) {
    Vcd::Scanner scan{path, bufferSize};
    std::vector<std::string> out;
    while (true) {
        const std::string_view t = scan.next();
        if (t.empty())
            break;
        out.emplace_back(t);
    }
    return out;
}

}  // namespace

// Крошечный буфер заставляет каждый токен пересекать границу блока: результат
// обязан совпадать с разбором одним куском.
TEST(Scanner, SurvivesChunkBoundaries) {
    const TempText tmp{"wasafe_scan_chunks.txt", "$var wire 8 # data[7:0] $end\nb10100101 #\n#120\n0!\n"};
    const std::vector<std::string> expected{"$var", "wire", "8", "#", "data[7:0]", "$end", "b10100101", "#", "#120",
            "0!"};

    for (const std::size_t size :
            {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{16}, std::size_t{1024}})
        EXPECT_EQ(tokenize(tmp.path, size), expected) << "buffer " << size;
}

// Токен длиннее буфера: буфер должен вырасти, а токен — уцелеть.
TEST(Scanner, TokenLongerThanBuffer) {
    const std::string big(5000, 'x');
    const TempText tmp{"wasafe_scan_long.txt", "a " + big + " b"};

    const std::vector<std::string> expected{"a", big, "b"};
    EXPECT_EQ(tokenize(tmp.path, 4), expected);
    EXPECT_EQ(tokenize(tmp.path, 64), expected);
}

// Любые байты <= ' ' — разделители, в том числе CR из CRLF.
TEST(Scanner, TreatsAllWhitespaceAsSeparator) {
    const TempText tmp{"wasafe_scan_ws.txt", "\r\n  a\t\t b \r\n\r\n c\r\n"};
    const std::vector<std::string> expected{"a", "b", "c"};

    EXPECT_EQ(tokenize(tmp.path, 3), expected);
    EXPECT_EQ(tokenize(tmp.path, 4096), expected);
}

// Файл без завершающего перевода строки: последний токен не теряется.
TEST(Scanner, LastTokenWithoutTrailingNewline) {
    const TempText tmp{"wasafe_scan_tail.txt", "one two three"};
    const std::vector<std::string> expected{"one", "two", "three"};

    EXPECT_EQ(tokenize(tmp.path, 2), expected);
    EXPECT_EQ(tokenize(tmp.path, 1024), expected);
}

TEST(Scanner, EmptyAndBlankFiles) {
    const TempText empty{"wasafe_scan_empty.txt", ""};
    EXPECT_TRUE(tokenize(empty.path, 16).empty());

    const TempText blank{"wasafe_scan_blank.txt", "   \n\t\r\n  "};
    EXPECT_TRUE(tokenize(blank.path, 2).empty());
}

TEST(Scanner, ThrowsOnMissingFile) {
    const auto path = std::filesystem::temp_directory_path() / "wasafe_scan_absent.txt";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    EXPECT_THROW((void)Vcd::Scanner{path}, Vcd::Error);
}
