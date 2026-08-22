#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>

#include "emitter.hpp"
#include "wasafe_vcd/exception.hpp"

using namespace WaSafe;

namespace {

/// Временный файл или каталог с его последующим удалением.
struct FileGuard {
    explicit FileGuard(std::string_view name) : path(std::filesystem::temp_directory_path() / name) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    ~FileGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;

    std::filesystem::path path;
};

/// Содержимое файла целиком.
std::string readAll(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

// Маленький буфер заставляет записи пересекать его границу: перед одними буфер
// сбрасывается в файл, под другие ещё и вырастает. При любом размере буфера
// файл обязан совпасть со склейкой записей; размер 1024 — контрольный, там всё
// умещается сразу.
TEST(Emitter, SurvivesChunkBoundaries) {
    // Длинные записи идут последними: рост буфера необратим, и после него
    // короткие записи уже не пересекали бы его границу.
    constexpr std::array<std::string_view, 5> texts{"a", "bc", "def", "0123456789", "very very long long long text"};
    const auto expected = texts | std::views::join | std::ranges::to<std::string>();

    for (const std::size_t size : {1, 2, 3, 7, 16, 1024}) {
        const FileGuard file{std::format("wasafe_emit_chunks_{}.txt", size)};
        Vcd::Emitter emitter{file.path, size};
        for (const std::string_view str : texts) {
            char* data = emitter.reserve(str.size());
            std::ranges::copy(str, data);
            emitter.commit(data + str.size());
        }
        emitter.finish();

        EXPECT_EQ(readAll(file.path), expected) << "buffer " << size;
    }
}

// text() — не горячий путь, но именно он пишет заголовок: соседние вызовы
// обязаны склеиваться встык, а не затирать друг друга.
TEST(Emitter, AppendsSuccessiveTextWrites) {
    for (const std::size_t size : {1, 4, 16, 1024}) {
        const FileGuard file{std::format("wasafe_emit_text_{}.txt", size)};
        {
            Vcd::Emitter emitter{file.path, size};
            emitter.text("$version ");
            emitter.text("wasafe-vcd 1.0");
            emitter.text(" $end\n");
            emitter.finish();
        }
        EXPECT_EQ("$version wasafe-vcd 1.0 $end\n", readAll(file.path));
    }
}

// Запись длиннее всего буфера обязана выращивать его, а не рваться на части.
TEST(Emitter, GrowsBufferForOversizedRecord) {
    const FileGuard file{"wasafe_emit_oversized.txt"};
    const std::string big(10'000, 'x');
    {
        Vcd::Emitter emitter{file.path, 8};
        char* data = emitter.reserve(big.size());
        std::ranges::copy(big, data);
        emitter.commit(data + big.size());
        emitter.finish();
    }
    EXPECT_EQ(big, readAll(file.path));
}

// Данные обязаны быть на диске после finish(), а не после деструктора:
// finish() — единственная точка успешного пути.
TEST(Emitter, FlushesAndClosesOnFinish) {
    const FileGuard file{"wasafe_emit_finish.txt"};
    Vcd::Emitter emitter{file.path, Vcd::Emitter::kDefaultBufferSize};
    emitter.text("payload");
    emitter.finish();

    EXPECT_EQ("payload", readAll(file.path));  // emitter ещё жив
}

TEST(Emitter, ReportsUnopenablePath) {
    const auto path = std::filesystem::temp_directory_path() / "wasafe_emit_no_such_dir" / "out.vcd";
    EXPECT_THROW(Vcd::Emitter{path}, Vcd::Error);
}

TEST(Emitter, KeepsOldFileWhenNotFinished) {
    constexpr std::string_view verificationString{"correct file"};

    const FileGuard dir{"wasafe_emit_unfinished"};
    const std::filesystem::path file = dir.path / "out.txt";
    std::filesystem::remove_all(dir.path);
    std::filesystem::create_directories(dir.path);

    {
        std::ofstream f{file, std::ios_base::binary};
        ASSERT_TRUE(f);
        f << verificationString;
    }
    {
        Vcd::Emitter emitter{file};
        emitter.text("trash");
    }

    EXPECT_EQ(verificationString, readAll(file));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{dir.path}, {}), 1);
}

// rename не удался (цель — непустой каталог): бросается Vcd::Error, а не
// filesystem_error, и временный файл не остаётся.
TEST(Emitter, ReportsFailedReplace) {
    const FileGuard dir{"wasafe_emit_replace"};
    std::filesystem::remove_all(dir.path);
    const auto target = dir.path / "out.vcd";
    std::filesystem::create_directories(target / "sub");
    {
        Vcd::Emitter emitter{target};
        emitter.text("data");
        EXPECT_THROW(emitter.finish(), Vcd::Error);
    }
    // Уборка — в деструкторе, поэтому проверка после блока.
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{dir.path}, {}), 1);  // только out.vcd/
}
