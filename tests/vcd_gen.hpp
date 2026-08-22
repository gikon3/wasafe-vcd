#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

/// Генератор большого синтетического VCD с ДЕТЕРМИНИРОВАННЫМИ значениями:
/// один и тот же (сигнал, шаг) всегда даёт одно и то же значение, поэтому
/// разобранный дамп можно сверять с формулой, не храня эталон.
namespace VcdGen {

inline constexpr std::uint32_t kScalars = 128;
inline constexpr std::uint32_t kVectors = 96;
inline constexpr std::uint32_t kReals = 32;
inline constexpr std::uint32_t kSignals = kScalars + kVectors + kReals;

/// Ширина i-го вектора: 8/16/32/64 по кругу.
[[nodiscard]] inline std::uint32_t vectorWidth(std::uint32_t i) {
    constexpr std::array<std::uint32_t, 4> kWidths{8, 16, 32, 64};
    return kWidths[i % kWidths.size()];
}

/// VCD-код сигнала: последовательность в алфавите печатных ASCII 33..126,
/// как её порождают реальные симуляторы (сначала однобайтовые, потом длиннее).
[[nodiscard]] inline std::string code(std::uint32_t index) {
    std::string s;
    for (std::uint32_t v = index;; v /= 94) {
        s.push_back(static_cast<char>('!' + (v % 94)));
        if (v < 94)
            return s;
    }
}

// --- формулы значений -------------------------------------------------------

/// Скаляр: чередование 0/1 с редкими 'x' — чтобы четырёхзначный путь тоже жил.
[[nodiscard]] inline char scalarAt(std::uint32_t i, std::uint64_t step) {
    const std::uint64_t mix = step * 31 + i;
    if (mix % 101 == 0)
        return 'x';
    return static_cast<char>('0' + (mix & 1));
}

[[nodiscard]] inline std::uint64_t vectorAt(std::uint32_t i, std::uint64_t step) {
    const std::uint64_t v = step * 6364136223846793005ULL + std::uint64_t{i} * 2654435761ULL;
    const std::uint32_t w = vectorWidth(i);
    return w == 64 ? v : v & ((std::uint64_t{1} << w) - 1);
}

/// Значение подобрано так, чтобы точно печататься с точностью потока по
/// умолчанию (6 значащих цифр) — иначе сверка после чтения была бы неточной.
[[nodiscard]] inline double realAt(std::uint32_t i, std::uint64_t step) {
    return static_cast<double>(step % 1000) * 0.25 + static_cast<double>(i);
}

/// Полная строка вектора шириной w (MSB слева) — то, что должен вернуть toString().
[[nodiscard]] inline std::string vectorString(std::uint32_t i, std::uint64_t step) {
    const std::uint32_t w = vectorWidth(i);
    const std::uint64_t v = vectorAt(i, step);
    std::string s(w, '0');
    for (std::uint32_t bit = 0; bit < w; ++bit)
        s[w - 1 - bit] = static_cast<char>('0' + ((v >> bit) & 1U));
    return s;
}

/// Тот же вектор, но как его пишет симулятор: без ведущих нулей (это заодно
/// прогоняет правило расширения при чтении).
[[nodiscard]] inline std::string vectorLiteral(std::uint32_t i, std::uint64_t step) {
    const std::string s = vectorString(i, step);
    const std::size_t first = s.find('1');
    return first == std::string::npos ? std::string{"0"} : s.substr(first);
}

// --- имена ------------------------------------------------------------------

[[nodiscard]] inline std::string scalarName(std::uint32_t i) {
    return "top.sc_" + std::to_string(i);
}
[[nodiscard]] inline std::string vectorName(std::uint32_t i) {
    return "top.vec_" + std::to_string(i);
}
[[nodiscard]] inline std::string realName(std::uint32_t i) {
    return "top.re_" + std::to_string(i);
}

/// Записать дамп, пока файл не дорастёт до targetBytes. Возвращает число шагов.
inline std::uint64_t write(const std::filesystem::path& path, std::uint64_t targetBytes) {
    std::ofstream out(path, std::ios::binary);
    std::vector<char> buf(1U << 20U);
    out.rdbuf()->pubsetbuf(buf.data(), static_cast<std::streamsize>(buf.size()));

    out << "$date Fri $end\n$version vcd_gen $end\n$timescale 1ns $end\n$scope module top $end\n";
    for (std::uint32_t i = 0; i < kScalars; ++i)
        out << "$var wire 1 " << code(i) << " sc_" << i << " $end\n";
    for (std::uint32_t i = 0; i < kVectors; ++i) {
        const std::uint32_t w = vectorWidth(i);
        out << "$var wire " << w << ' ' << code(kScalars + i) << " vec_" << i << " [" << (w - 1) << ":0] $end\n";
    }
    for (std::uint32_t i = 0; i < kReals; ++i)
        out << "$var real 64 " << code(kScalars + kVectors + i) << " re_" << i << " $end\n";
    out << "$upscope $end\n$enddefinitions $end\n";

    std::uint64_t step = 0;
    while (true) {
        out << '#' << step << '\n';
        for (std::uint32_t i = 0; i < kScalars; ++i)
            out << scalarAt(i, step) << code(i) << '\n';
        for (std::uint32_t i = 0; i < kVectors; ++i)
            out << 'b' << vectorLiteral(i, step) << ' ' << code(kScalars + i) << '\n';
        for (std::uint32_t i = 0; i < kReals; ++i)
            out << 'r' << realAt(i, step) << ' ' << code(kScalars + kVectors + i) << '\n';
        ++step;
        if ((step & 0xFFU) == 0 && static_cast<std::uint64_t>(out.tellp()) >= targetBytes)
            break;
    }
    out.flush();
    return step;
}

/// Файл с автоудалением (вместе с возможными сайдкарами store/index).
struct TempFile {
    std::filesystem::path path;

    explicit TempFile(std::string_view name) : path(std::filesystem::temp_directory_path() / name) {}
    TempFile(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(std::filesystem::path(path) += ".wsfstore", ec);
    }

    TempFile& operator=(const TempFile&) = delete;
    TempFile& operator=(TempFile&&) = delete;
};

}  // namespace VcdGen
