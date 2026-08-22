#pragma once

#include <wasafe/io/writer.hpp>

#include "wasafe_vcd/export.hpp"

namespace WaSafe::Vcd {

/// Экспорт БД в VCD.
class WASAFE_VCD_API VcdWriter final : public Writer {
public:
    [[nodiscard]] std::string_view format() const override { return "vcd"; }
    void write(const Database& db, const std::filesystem::path& path) override;
};

}  // namespace WaSafe::Vcd
