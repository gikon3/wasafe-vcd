#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>
#include <wasafe/types/logic.hpp>
#include <wasafe/types/logic_vector_view.hpp>

namespace WaSafe::Vcd {

/// Значение, которым VCD расширяет вектор влево до объявленной ширины
/// (IEEE 1364): 0/1 — нулями, x — иксами, z — z-состоянием.
[[nodiscard]] inline Logic extensionLogic(char msb) {
    switch (msb) {
        case 'x':
        case 'X':
            return Logic::X;
        case 'z':
        case 'Z':
            return Logic::Z;
        default:
            return Logic::ZERO;
    }
}

/// Быстрый путь для однобитового сигнала — мимо декодера и выравнивания.
[[nodiscard]] inline LogicVectorView singleBitView(Logic value) {
    static constexpr std::array<std::uint64_t, 2> kWord{0, 1};
    return LogicVectorView{&kWord[logicA(value)], logicB(value) != 0 ? &kWord[1] : nullptr, 1};
}

// --- пакетное чтение битов (SWAR) -------------------------------------------
// Восемь подряд идущих ASCII-символов читаются одним словом. Приём работает
// только на little-endian: там байт с индексом j лежит в разрядах 8j.
inline constexpr std::size_t kSwarChars = 8;
inline constexpr bool kSwarUsable = std::endian::native == std::endian::little;
inline constexpr std::uint64_t kByteOnes = 0x0101'0101'0101'0101ULL;
inline constexpr std::uint64_t kByteZeroDigits = 0x3030'3030'3030'3030ULL;
/// Множитель, собирающий младшие биты восьми байт в один байт так, что первый
/// байт в памяти (старший разряд значения) становится седьмым битом.
inline constexpr std::uint64_t kByteGather = 0x8040'2010'0804'0201ULL;

/// true, если все восемь символов — '0' или '1'.
[[nodiscard]] inline bool isTwoStateChunk(std::uint64_t chunk) {
    return kSwarUsable && (chunk & ~kByteOnes) == kByteZeroDigits;
}

/// Восемь символов '0'/'1' -> восемь бит (MSB слева, как в VCD).
[[nodiscard]] inline std::uint64_t packTwoStateChunk(std::uint64_t chunk) {
    return ((chunk & kByteOnes) * kByteGather) >> 56U;
}

/// Бит-планы одного разбираемого значения. Живёт всю жизнь парсера и
/// переиспользуется между изменениями, поэтому на горячем пути аллокаций нет.
///
/// Весь класс определён в заголовке намеренно: decode()/view() зовутся на
/// каждое изменение вектора (десятки миллионов раз на большом дампе), а
/// межмодульного инлайнинга в сборке без LTO нет.
class ValueBuffer final {
public:
    /// Разобрать строку битов (MSB слева) в бит-планы.
    void decode(std::string_view bits) {
        const auto width = static_cast<std::uint32_t>(bits.size());
        valueWidth_ = width;
        ensureWords(LogicVectorView::wordsFor(width));

        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::uint64_t anyB = 0;
        std::uint32_t bit = 0;
        std::uint32_t word = 0;
        std::size_t k = bits.size();  // конец ещё не разобранного суффикса
        while (k > 0) {
            // Быстрый путь: восьмёрка символов из одних '0'/'1' упаковывается в
            // байт без ветвлений (в дампах такие значения абсолютно преобладают).
            if (k >= kSwarChars && bit <= LogicVectorView::kWordWidth - kSwarChars) {
                std::uint64_t chunk = 0;
                std::memcpy(&chunk, bits.data() + k - kSwarChars, sizeof(chunk));
                if (isTwoStateChunk(chunk)) {
                    a |= packTwoStateChunk(chunk) << bit;
                    bit += kSwarChars;
                    k -= kSwarChars;
                    if (bit == LogicVectorView::kWordWidth) {
                        aval_[word] = a;
                        bval_[word] = b;
                        anyB |= b;
                        a = 0;
                        b = 0;
                        bit = 0;
                        ++word;
                    }
                    continue;
                }
            }

            const Logic g = logicFromChar(bits[--k]);  // младший разряд — справа
            a |= static_cast<std::uint64_t>(logicA(g)) << bit;
            b |= static_cast<std::uint64_t>(logicB(g)) << bit;
            if (++bit == LogicVectorView::kWordWidth) {
                aval_[word] = a;
                bval_[word] = b;
                anyB |= b;
                a = 0;
                b = 0;
                bit = 0;
                ++word;
            }
        }
        if (bit != 0) {
            aval_[word] = a;
            bval_[word] = b;
            anyB |= b;
        }
        fourState_ = anyB != 0;
    }

    /// Привести разобранное значение к ширине сигнала и отдать его видом.
    /// Расширение слева идёт значением `ext`, лишние старшие разряды
    /// отбрасываются.
    [[nodiscard]] LogicVectorView view(std::uint32_t width, Logic ext) {
        adjustWidth(width, ext);
        return LogicVectorView{aval_.data(), fourState_ ? bval_.data() : nullptr, width};
    }

private:
    void adjustWidth(std::uint32_t target, Logic ext) {
        if (target == valueWidth_)
            return;

        const std::uint32_t words = LogicVectorView::wordsFor(target);
        ensureWords(words);
        if (target > valueWidth_) {
            for (std::uint32_t w = LogicVectorView::wordsFor(valueWidth_); w < words; ++w) {
                aval_[w] = 0;
                bval_[w] = 0;
            }
            fillRange(valueWidth_, target, ext);
            if (logicB(ext) != 0)
                fourState_ = true;
        }
        else {
            // Хвост последнего слова обязан быть нулевым: на этом держатся
            // сравнение, toUint64 и упаковка значения в storage.
            if (const std::uint32_t rem = target % LogicVectorView::kWordWidth; rem != 0) {
                const std::uint64_t mask = (std::uint64_t{1} << rem) - 1;
                aval_[words - 1] &= mask;
                bval_[words - 1] &= mask;
            }
            if (fourState_) {  // усечение могло убрать все x/z
                std::uint64_t anyB = 0;
                for (std::uint32_t w = 0; w < words; ++w)
                    anyB |= bval_[w];
                fourState_ = anyB != 0;
            }
        }
        valueWidth_ = target;
    }

    void fillRange(std::uint32_t from, std::uint32_t to, Logic ext) {
        const std::uint64_t patternA = logicA(ext) != 0 ? ~std::uint64_t{0} : 0;
        const std::uint64_t patternB = logicB(ext) != 0 ? ~std::uint64_t{0} : 0;
        for (std::uint32_t bit = from; bit < to;) {
            const std::uint32_t word = bit / LogicVectorView::kWordWidth;
            const std::uint32_t off = bit % LogicVectorView::kWordWidth;
            const std::uint32_t count = std::min(LogicVectorView::kWordWidth - off, to - bit);
            const std::uint64_t mask = count == LogicVectorView::kWordWidth
                    ? ~std::uint64_t{0}
                    : (((std::uint64_t{1} << count) - 1) << off);
            aval_[word] = (aval_[word] & ~mask) | (patternA & mask);
            bval_[word] = (bval_[word] & ~mask) | (patternB & mask);
            bit += count;
        }
    }

    void ensureWords(std::uint32_t words) {
        if (aval_.size() < words) {
            aval_.resize(words);
            bval_.resize(words);
        }
    }

private:
    std::vector<std::uint64_t> aval_;
    std::vector<std::uint64_t> bval_;
    std::uint32_t valueWidth_ = 0;
    bool fourState_ = false;
};

}  // namespace WaSafe::Vcd
