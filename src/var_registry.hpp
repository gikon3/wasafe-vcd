#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <wasafe/io/builder.hpp>

#include "code_table.hpp"
#include "vcd_syntax.hpp"

namespace WaSafe::Vcd {

/// Накопитель объявлений $var с отложенным сбросом по границе scope.
///
/// Решение «обычный сигнал или элемент шины» принимается, лишь когда виден весь
/// уровень: раньше неизвестны ни границы измерений массива, ни конфликты имён.
/// Поэтому add() только буферизует запись, а объявления в Builder уходят из
/// flushLevel()/popLevel().
///
/// Владеет таблицей кодов, потому что она наполняется здесь же — в момент
/// объявления. Поток значений обращается к ней через find(), и только этот
/// метод лежит на горячем пути; вся остальная работа класса идёт раз на scope.
class VarRegistry final {
public:
    explicit VarRegistry(Builder& sink);
    VarRegistry(const VarRegistry&) = delete;
    VarRegistry(VarRegistry&&) = delete;
    ~VarRegistry() = default;

    /// Найти сигнал по коду VCD; nullptr — код не объявлялся.
    [[nodiscard]] const VarInfo* find(std::string_view code) const { return codes_.find(code); }

    /// Глубина стека уровней; 1 — только корневой.
    [[nodiscard]] std::size_t depth() const { return levels_.size(); }

    /// Отложить $var на текущем уровне. Текст копируется в арену уровня, так что
    /// переданные view могут указывать в буфер сканера.
    void add(VarType vt, std::uint32_t size, std::string_view code, std::string_view ref);

    void pushLevel();

    /// Объявить всё, что накоплено на текущем уровне, в порядке появления.
    void flushLevel();

    /// Сбросить уровень и снять его со стека, если он не корневой.
    void popLevel();

    VarRegistry& operator=(const VarRegistry&) = delete;
    VarRegistry& operator=(VarRegistry&&) = delete;

private:
    /// Разобранный $var, отложенный до конца своего scope.
    ///
    /// Текст и индексы лежат в аренах уровня, а сама запись — POD: буферизация
    /// заголовка не делает ни одной аллокации на переменную.
    struct PendingVar {
        std::uint32_t refPos = 0;  ///< начало ссылки в Level::text
        std::uint32_t refLen = 0;
        std::uint32_t codePos = 0;  ///< начало кода в Level::text
        std::uint32_t codeLen = 0;
        std::uint32_t indexPos = 0;    ///< начало индексов в Level::indices
        std::uint32_t indexCount = 0;  ///< 0 у обычного $var
        std::uint32_t baseLen = 0;     ///< длина базового имени внутри ссылки
        std::uint32_t litLen = 0;      ///< длина имени с индексами, но без диапазона
        LeafSpec leaf;
    };

    /// Один уровень scope: отложенные записи и арены под их текст и индексы.
    /// Во время сброса арены не растут — на них смотрят string_view.
    struct Level {
        std::vector<PendingVar> vars;
        std::string text;
        std::vector<std::int32_t> indices;

        [[nodiscard]] std::string_view code(const PendingVar& v) const {
            return std::string_view{text}.substr(v.codePos, v.codeLen);
        }
        /// Базовое имя — без индексов и без диапазона.
        [[nodiscard]] std::string_view base(const PendingVar& v) const {
            return std::string_view{text}.substr(v.refPos, v.baseLen);
        }
        /// Имя, под которым $var объявляется, если шина не сложилась.
        [[nodiscard]] std::string_view literal(const PendingVar& v) const {
            return std::string_view{text}.substr(v.refPos, v.litLen);
        }
        [[nodiscard]] std::span<const std::int32_t> idx(const PendingVar& v) const {
            return std::span{indices}.subspan(v.indexPos, v.indexCount);
        }

        void clear() {
            vars.clear();
            text.clear();
            indices.clear();
        }
    };

    /// Одно измерение шины. Направление берётся из порядка появления индексов в
    /// дампе: VCD его не хранит, а порядок хотя бы воспроизводим.
    struct Dim {
        std::int32_t left = 0;
        std::int32_t right = 0;

        [[nodiscard]] std::size_t count() const {
            return static_cast<std::size_t>(std::abs(static_cast<std::int64_t>(left) - right) + 1);
        }
        [[nodiscard]] std::size_t ordinalOf(std::int32_t index) const {
            return static_cast<std::size_t>(
                    left >= right ? static_cast<std::int64_t>(left) - index : static_cast<std::int64_t>(index) - left);
        }
    };

    /// Кандидат в шину: все $var уровня с одним базовым именем.
    struct Group {
        std::vector<std::size_t> members;   ///< индексы в буфере уровня
        std::vector<std::size_t> ordinals;  ///< порядковый номер элемента, параллельно members
        std::vector<Dim> dims;
        LeafSpec leaf;
        bool merge = false;  ///< объединять ли в шину
        bool done = false;   ///< шина уже объявлена
    };

private:
    /// Обычный сигнал (или элемент необъединённой группы — под литеральным именем).
    void declarePlain(const Level& level, const PendingVar& var, std::string_view name);

    /// Решить, становится ли группа шиной, и посчитать её измерения.
    void prepareGroup(const Level& level, Group& group, const std::unordered_set<std::string_view>& plain);

    /// Объявить шину и раздать её потоки кодам элементов.
    void materializeGroup(const Level& level, const Group& group);

private:
    Builder& sink_;
    CodeTable codes_;

    /// Отложенные $var по уровням scope; синхронен со стеком scope в builder.
    std::vector<Level> levels_;

    // Scratch под разбор одной группы: переиспользуется между группами, чтобы
    // заголовок из десятков тысяч шин не стоил аллокаций на каждую.
    std::vector<std::int32_t> indexScratch_;
    std::vector<std::int32_t> dimLo_;
    std::vector<std::int32_t> dimHi_;
    std::vector<std::int32_t> dimLast_;
    std::unordered_set<std::string_view> freshCodes_;
    std::vector<std::uint8_t> taken_;
};

}  // namespace WaSafe::Vcd
