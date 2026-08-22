#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <wasafe/core/exception.hpp>
#include <wasafe/io/builder.hpp>
#include <wasafe/model/database.hpp>
#include <wasafe/types/type.hpp>

/// Приёмник, не хранящий ничего: изолирует стоимость РАЗБОРА от стоимости
/// построения хранилища. Считает изменения, чтобы разбор нельзя было выкинуть.
/// Общий для функционального теста на объёме и для бенчмарка.
class CountingBuilder final : public WaSafe::Builder {
public:
    void setTimeScale(WaSafe::TimeScale /*scale*/) override {}
    WaSafe::ScopeId beginScope(std::string_view /*name*/, WaSafe::ScopeKind /*kind*/) override {
        return WaSafe::ScopeId{0};
    }
    void endScope() override {}

    WaSafe::SignalId declareVar(std::string_view /*name*/, WaSafe::Type type, std::optional<WaSafe::SignalId> alias,
            std::span<WaSafe::SignalId> leaves) override {
        // Явная привязка листьев: невалидные слоты получают свежие id — ровно
        // то, что делает настоящий builder, только без хранилища.
        for (WaSafe::SignalId& leaf : leaves) {
            if (!leaf.valid())
                leaf = WaSafe::SignalId{next_++};
        }
        if (alias)
            return *alias;
        return WaSafe::singleStreamRepresentable(type) ? WaSafe::SignalId{next_++} : WaSafe::SignalId{};
    }

    void headerDone() override {}
    void setTime(WaSafe::TimeStamp /*time*/) override { ++times_; }
    void valueChange(WaSafe::SignalId /*id*/, WaSafe::ValueView /*value*/) override { ++changes_; }
    void finish() override {}
    WaSafe::Database takeDatabase() override { throw WaSafe::Exception{"CountingBuilder has no database"}; }

    [[nodiscard]] std::uint64_t changes() const noexcept { return changes_; }
    [[nodiscard]] std::uint64_t times() const noexcept { return times_; }

private:
    std::uint32_t next_ = 0;
    std::uint64_t changes_ = 0;
    std::uint64_t times_ = 0;
};
