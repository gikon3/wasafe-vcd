#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include <wasafe/core/ids.hpp>

namespace WaSafe::Vcd {

/// Коды VCD для потоков выгружаемой БД. Код выдаётся на поток (SignalId), а не
/// на узел: алиасы получают код первого объявления. Горячий путь — code() по
/// порядковому номеру потока, который совпадает с индексом source в курсоре.
class CodeBook final {
public:
    /// Результат assign(): код потока и его порядковый номер.
    struct Assignment {
        std::string_view code;      ///< валиден до следующего assign()
        std::uint32_t ordinal = 0;  ///< номер потока в порядке первого assign(); ключ для code()
        bool fresh = false;         ///< поток встретился впервые — код только что выдан
    };

public:
    /// Заранее развернуть арену и смещения под ожидаемое число потоков.
    void reserve(std::size_t streams) {
        // У каждого кода есть первый символ; коды с номерами от 94^k и выше
        // получают ещё по символу за каждое такое k >= 1.
        std::size_t chars = streams;
        for (std::size_t bound = kAlphabet; bound < streams; bound *= kAlphabet)
            chars += streams - bound;
        arena_.reserve(chars);
        offsets_.reserve(streams + 1);
    }

    /// Код потока: существующий, если поток уже встречался (алиас), иначе
    /// следующий по порядку.
    [[nodiscard]] Assignment assign(SignalId id) {
        const auto index = id.get();
        if (index >= ordinalById_.size())
            ordinalById_.resize(index + 1, kNone);

        if (const auto known = ordinalById_[index]; known != kNone) {
            const auto o = static_cast<std::uint32_t>(known);
            return {code(o), o, false};
        }

        const auto o = static_cast<std::uint32_t>(size());
        std::array<char, kMaxCodeChars> buf{};
        const auto len = encode(o, buf.data());
        arena_.insert(arena_.end(), buf.data(), buf.data() + len);
        offsets_.push_back(static_cast<std::uint32_t>(arena_.size()));
        ordinalById_[index] = static_cast<std::int32_t>(o);
        return {code(o), o, true};
    }

    /// Код по порядковому номеру потока. Без проверок: ordinal < size().
    [[nodiscard]] std::string_view code(std::uint32_t ordinal) const noexcept {
        return {arena_.data() + offsets_[ordinal], offsets_[ordinal + 1] - offsets_[ordinal]};
    }

    /// Число выданных кодов (уникальных потоков).
    [[nodiscard]] std::size_t size() const noexcept { return offsets_.size() - 1; }

private:
    /// Записать код с номером ordinal в dst; вернуть длину. Код запишется в dst:
    /// цифры в основании 94 от младшей, алфавит '!'..'~'.
    static std::size_t encode(std::uint32_t ordinal, char* dst) {
        std::size_t n = 0;
        while (true) {
            dst[n++] = static_cast<char>('!' + ordinal % kAlphabet);
            ordinal /= kAlphabet;
            if (ordinal == 0)
                return n;
        }
    }

private:
    static constexpr std::int32_t kNone = -1;
    static constexpr std::uint32_t kAlphabet = '~' - '!' + 1;  ///< 94
    static constexpr std::size_t kMaxCodeChars = 5;            ///< 94^5 > 2^32

private:
    std::vector<char> arena_;                ///< коды подряд, без разделителей
    std::vector<std::uint32_t> offsets_{0};  ///< code(i) = arena_[offsets_[i], offsets_[i+1])
    std::vector<std::int32_t> ordinalById_;  ///< SignalId::get() -> ordinal либо kNone
};

}  // namespace WaSafe::Vcd
