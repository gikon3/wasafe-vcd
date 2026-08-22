/// Бенчмарк разбора и выгрузки VCD: отдельный бинарь, чтобы в профиле не было
/// обвязки тестового фреймворка и чтобы размер дампа задавался аргументом.
///
///   wasafe_vcd_bench [--mb=N] [--repeat=N] [--stage=parse|ingest|write|all]
///                    [--input=PATH] [--keep]
///
/// Проверок здесь нет: корректность на объёме проверяет тест VcdLarge.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <wasafe/io/builder.hpp>
#include <wasafe/io/ingest.hpp>
#include <wasafe/model/database.hpp>

#include "counting_builder.hpp"
#include "vcd_gen.hpp"
#include "wasafe_vcd/reader.hpp"
#include "wasafe_vcd/writer.hpp"

using namespace WaSafe;
using Seconds = std::chrono::duration<double>;

namespace {

/// Ширина колонки с названием этапа — только ради читаемого вывода.
constexpr int kLabelWidth = 11;

struct Options {
    std::uint64_t mb = 128;
    std::uint32_t repeat = 1;
    std::string stage = "all";
    std::string input;
    bool keep = false;
};

[[nodiscard]] bool parseUint(std::string_view text, std::uint64_t& out) {
    return std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc{} && out != 0;
}

void usage(std::ostream& os) {
    os << "usage: wasafe_vcd_bench [--mb=N] [--repeat=N] [--stage=parse|ingest|write|all] [--input=PATH] [--keep]\n"
       << "  --mb      размер синтетического дампа в МиБ (по умолчанию 128)\n"
       << "  --repeat  число прогонов каждого этапа; печатается лучший (по умолчанию 1)\n"
       << "  --stage   что мерить: только разбор, сквозной ingest, выгрузку writer'ом или всё\n"
       << "  --input   взять готовый .vcd вместо генерации\n"
       << "  --keep    не удалять сгенерированный дамп (для повторных запусков через --input)\n";
}

/// Разбор аргументов. Возвращает nullopt, если работать не нужно (--help или ошибка).
[[nodiscard]] std::optional<Options> parseArgs(int argc, const char* const* argv, int& exitCode) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        std::uint64_t value = 0;
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return std::nullopt;
        }
        if (arg.starts_with("--mb=") && parseUint(arg.substr(5), value)) {
            opts.mb = value;
        }
        else if (arg.starts_with("--repeat=") && parseUint(arg.substr(9), value)) {
            opts.repeat = static_cast<std::uint32_t>(value);
        }
        else if (arg.starts_with("--stage=")) {
            opts.stage = std::string{arg.substr(8)};
            if (opts.stage != "parse" && opts.stage != "ingest" && opts.stage != "write" && opts.stage != "all") {
                std::cerr << "неизвестный этап: " << opts.stage << '\n';
                exitCode = 2;
                return std::nullopt;
            }
        }
        else if (arg.starts_with("--input=")) {
            opts.input = std::string{arg.substr(8)};
        }
        else if (arg == "--keep") {
            opts.keep = true;
        }
        else {
            std::cerr << "неизвестный аргумент: " << arg << '\n';
            usage(std::cerr);
            exitCode = 2;
            return std::nullopt;
        }
    }
    return opts;
}

/// Мегабайты в секунду по времени `d` для файла размером `bytes`.
[[nodiscard]] double mbPerSec(std::uint64_t bytes, Seconds d) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / d.count();
}

/// Строка результата этапа: лучший прогон плюс медиана, если прогонов было
/// несколько. Один прогон нельзя сравнивать между сборками — шум того же
/// порядка, что и разница, ради которой обычно и запускают бенчмарк.
void report(std::string_view name, std::uint64_t bytes, std::vector<Seconds> times) {
    std::ranges::sort(times);
    const Seconds best = times.front();
    std::cout << std::left << std::setw(kLabelWidth) << name << ": " << best.count() << " s, " << mbPerSec(bytes, best)
              << " MiB/s";
    if (times.size() > 1)
        std::cout << " (лучший из " << times.size() << ", медиана " << times[times.size() / 2].count() << " s)";
    std::cout << '\n';
}

/// Только разбор: приёмник ничего не хранит, поэтому в замер попадает
/// исключительно стоимость сканера и reader'а.
[[nodiscard]] Seconds runParse(const std::filesystem::path& path, CountingBuilder& sink) {
    Vcd::VcdReader reader{path};
    const auto start = std::chrono::steady_clock::now();
    reader.read(sink);
    return std::chrono::steady_clock::now() - start;
}

/// Сквозной путь: разбор плюс построение блочного хранилища на диске.
[[nodiscard]] Seconds runIngest(const std::filesystem::path& path) {
    const auto store = std::filesystem::path(path) += ".wsfstore";
    Vcd::VcdReader reader{path};
    auto builder = makeIndexingBuilder(store);
    const auto start = std::chrono::steady_clock::now();
    const Database db = ingest(reader, *builder);
    const Seconds d = std::chrono::steady_clock::now() - start;

    // Хранилище — производный файл, между прогонами его не переиспользуют.
    std::error_code ec;
    std::filesystem::remove(store, ec);
    return d;
}

/// Только выгрузка: БД уже в памяти, поэтому в замер попадает обход иерархии,
/// курсор по хранилищу и форматирование, а не разбор исходного дампа.
[[nodiscard]] Seconds runWrite(const Database& db, const std::filesystem::path& out) {
    Vcd::VcdWriter writer;
    const auto start = std::chrono::steady_clock::now();
    writer.write(db, out);
    return std::chrono::steady_clock::now() - start;
}

[[nodiscard]] bool wants(const Options& opts, std::string_view stage) {
    return opts.stage == "all" || opts.stage == stage;
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness) — сигнатуру main задаёт стандарт
int main(int argc, char** argv) {
    int exitCode = 0;
    const auto parsed = parseArgs(argc, argv, exitCode);
    if (!parsed)
        return exitCode;
    const Options& opts = *parsed;

    try {
        // Владение временным дампом: с --keep файл остаётся на диске, чтобы
        // следующий запуск взял его через --input и не тратил время на генерацию.
        std::optional<VcdGen::TempFile> owned;
        std::filesystem::path path{opts.input};
        std::uint64_t steps = 0;

        if (opts.input.empty()) {
            constexpr std::string_view kName = "wasafe_bench.vcd";
            path = std::filesystem::temp_directory_path() / kName;
            if (!opts.keep)
                owned.emplace(kName);

            const auto start = std::chrono::steady_clock::now();
            steps = VcdGen::write(path, opts.mb << 20U);
            const Seconds d = std::chrono::steady_clock::now() - start;
            std::cout << std::left << std::setw(kLabelWidth) << "generate" << ": " << steps << " шагов за " << d.count()
                      << " s -> " << path << '\n';
        }
        else if (!std::filesystem::exists(path)) {
            std::cerr << "нет такого файла: " << path << '\n';
            return 1;
        }

        const auto bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path));
        std::cout << std::left << std::setw(kLabelWidth) << "dump" << ": " << (bytes >> 20U) << " MiB\n";

        if (wants(opts, "parse")) {
            std::vector<Seconds> times;
            times.reserve(opts.repeat);
            std::uint64_t changes = 0;
            std::uint64_t stamps = 0;
            for (std::uint32_t i = 0; i < opts.repeat; ++i) {
                CountingBuilder sink;
                times.push_back(runParse(path, sink));
                changes = sink.changes();
                stamps = sink.times();
            }
            report("parse-only", bytes, std::move(times));
            std::cout << std::string(kLabelWidth + 2, ' ') << changes << " изменений, " << stamps << " меток времени\n";
        }

        if (wants(opts, "ingest")) {
            std::vector<Seconds> times;
            times.reserve(opts.repeat);
            for (std::uint32_t i = 0; i < opts.repeat; ++i)
                times.push_back(runIngest(path));
            report("ingest", bytes, std::move(times));
        }

        if (wants(opts, "write")) {
            // Разбор — подготовка, а не предмет замера.
            Vcd::VcdReader reader{path};
            auto builder = makeMemoryBuilder();
            const Database db = ingest(reader, *builder);

            const VcdGen::TempFile out{"wasafe_bench_written.vcd"};
            std::vector<Seconds> times;
            times.reserve(opts.repeat);
            for (std::uint32_t i = 0; i < opts.repeat; ++i)
                times.push_back(runWrite(db, out.path));

            // Скорость считается по выходу: его размер и есть объём работы writer'а.
            const auto written = static_cast<std::uint64_t>(std::filesystem::file_size(out.path));
            report("write", written, std::move(times));
            std::cout << std::string(kLabelWidth + 2, ' ') << (written >> 20U) << " MiB на выходе\n";
        }
    }
    catch (const std::exception& e) {
        std::cerr << "ошибка: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
