#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>
#include <wasafe/core/ids.hpp>
#include <wasafe/core/string_map.hpp>
#include <wasafe/types/value.hpp>

namespace WaSafe::Vcd {

/// Что известно про код VCD после разбора его первого $var.
struct VarInfo {
    SignalId id;
    std::uint32_t width = 0;            ///< ширина потока; значима только для LOGIC
    ValueKind kind = ValueKind::LOGIC;  ///< чем интерпретировать изменения этого кода
};

/// Таблица «код VCD -> сигнал», рассчитанная на горячий путь потока значений:
/// поиск идёт по string_view без создания std::string.
///
/// Коды — печатные ASCII 33..126 и почти всегда короткие, поэтому таблица
/// трёхуровневая: прямой массив для однобайтовых кодов (самый частый случай,
/// хеш не считается вовсе), хеш по упакованному в uint64 коду длиной до 8 байт
/// и обычная строковая карта для всего остального.
class CodeTable final {
public:
    /// Заранее развернуть таблицу под ожидаемое число кодов: рост по ходу
    /// разбора заголовка стоит перехеширования, а память здесь копеечная.
    void reserve(std::size_t vars) {
        vars_.reserve(vars);
        rehash(std::bit_ceil(std::max<std::size_t>(vars * 2, kMinSlots)));
    }

    /// Зарегистрировать код. Повторная регистрация игнорируется: в VCD это
    /// алиас, и поток остаётся за первым объявлением.
    void add(std::string_view code, const VarInfo& info) {
        if (code.empty() || find(code) != nullptr)
            return;
        const auto idx = static_cast<std::int32_t>(vars_.size());
        vars_.push_back(info);
        if (const std::size_t slot = singleSlot(code); slot != kNoSlot)
            single_[slot] = idx;
        else if (code.size() <= kPackedMax)
            insertPacked(pack(code), idx);
        else
            long_.emplace(code, idx);
    }

    /// Найти сигнал по коду; nullptr — код не объявлялся. Указатель живёт до
    /// следующего add() (правила инвалидации std::vector).
    [[nodiscard]] const VarInfo* find(std::string_view code) const {
        std::int32_t idx = kNone;
        if (const std::size_t slot = singleSlot(code); slot != kNoSlot)
            idx = single_[slot];
        else if (code.empty())
            idx = kNone;
        else if (code.size() <= kPackedMax)
            idx = findPacked(pack(code));
        else if (const auto it = long_.find(code); it != long_.end())
            idx = it->second;
        return idx == kNone ? nullptr : &vars_[static_cast<std::size_t>(idx)];
    }

private:
    /// Ячейка плоской хеш-таблицы; key == 0 означает «пусто» (упакованный код
    /// нулём быть не может — в алфавите VCD нет нулевого байта).
    struct Slot {
        std::uint64_t key = 0;
        std::int32_t idx = -1;
    };

private:
    /// Слот прямой таблицы для однобайтового кода из алфавита VCD; kNoSlot —
    /// код туда не укладывается и идёт в хеш.
    [[nodiscard]] static std::size_t singleSlot(std::string_view code) {
        if (code.size() != 1)
            return kNoSlot;
        const auto c = static_cast<unsigned>(static_cast<unsigned char>(code[0]));
        return c >= kFirstChar && c <= kLastChar ? c - kFirstChar : kNoSlot;
    }

    /// Код длиной до 8 байт как одно слово. Нулевых байтов в алфавите нет,
    /// поэтому коды разной длины не сталкиваются.
    [[nodiscard]] static std::uint64_t pack(std::string_view code) {
        std::uint64_t key = 0;
        // Порядок байтов важен только внутри одного процесса, так что достаточно простого memcpy.
        std::memcpy(&key, code.data(), code.size());  // code.size() <= kPackedMax
        return key;
    }

    /// Домашняя ячейка ключа (мультипликативное хеширование Кнута).
    ///
    /// Берутся именно СТАРШИЕ биты произведения: младший бит произведения равен
    /// младшему биту ключа, и чем разряд ниже, тем от меньшего числа битов
    /// ключа он зависит. Вырезать окно из середины фиксированным сдвигом —
    /// заманчиво, но ненадёжно: на последовательных кодах VCD такое окно теряет
    /// старшие байты кода и даёт вдвое больше коллизий, чем идеальный хеш.
    [[nodiscard]] std::size_t startSlot(std::uint64_t key) const {
        return static_cast<std::size_t>((key * kGolden) >> shift_);
    }

    /// Линейное пробирование: узловой unordered_map на горячем пути стоит
    /// лишней разыменовки, а здесь попадание почти всегда в одну строку кеша.
    [[nodiscard]] std::int32_t findPacked(std::uint64_t key) const {
        if (slots_.empty())
            return kNone;
        for (std::size_t i = startSlot(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key)
                return slots_[i].idx;
            if (slots_[i].key == 0)
                return kNone;
        }
    }

    /// Положить в свободную ячейку без проверки заполненности.
    void placeSlot(std::uint64_t key, std::int32_t idx) {
        for (std::size_t i = startSlot(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == 0) {
                slots_[i] = Slot{.key = key, .idx = idx};
                ++used_;
                return;
            }
        }
    }

    void insertPacked(std::uint64_t key, std::int32_t idx) {
        if (slots_.empty() || (used_ + 1) * 2 > slots_.size())
            rehash(slots_.empty() ? kMinSlots : slots_.size() * 2);
        placeSlot(key, idx);
    }

    void rehash(std::size_t slots) {
        if (slots <= slots_.size())
            return;
        const std::vector<Slot> old = std::move(slots_);
        slots_.assign(slots, Slot{});
        mask_ = slots - 1;
        shift_ = kWordBits - static_cast<unsigned>(std::countr_zero(slots));
        used_ = 0;
        for (const Slot& s : old) {
            if (s.key != 0)
                placeSlot(s.key, s.idx);
        }
    }

    /// Генерация массива из kSize одинаковых значений.
    template <std::size_t kSize, typename T>
    [[nodiscard]] static constexpr std::array<T, kSize> filled(const T& value) {
        std::array<T, kSize> arr{};
        arr.fill(value);
        return arr;
    }

private:
    static constexpr std::int32_t kNone = -1;
    static constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
    static constexpr unsigned kFirstChar = '!';  ///< алфавит кодов VCD — печатные ASCII
    static constexpr unsigned kLastChar = '~';
    static constexpr std::size_t kAlphabet = kLastChar - kFirstChar + 1;
    static constexpr std::size_t kPackedMax = 8;
    static constexpr std::size_t kMinSlots = 64;
    static constexpr std::uint64_t kGolden = 0x9E37'79B9'7F4A'7C15ULL;
    static constexpr unsigned kWordBits = std::numeric_limits<std::uint64_t>::digits;

private:
    std::vector<VarInfo> vars_;
    std::array<std::int32_t, kAlphabet> single_ = filled<kAlphabet>(kNone);
    std::vector<Slot> slots_;
    std::size_t mask_ = 0;            ///< slots_.size() - 1 (размер всегда степень двойки)
    unsigned shift_ = kWordBits - 1;  ///< 64 - log2(размер); задаётся в rehash() до первого поиска
    std::size_t used_ = 0;
    StringMap<std::int32_t> long_;
};

}  // namespace WaSafe::Vcd
