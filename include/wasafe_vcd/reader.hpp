#pragma once

#include <filesystem>
#include <string_view>
#include <wasafe/io/reader.hpp>

#include "wasafe_vcd/export.hpp"

namespace WaSafe::Vcd {

/// Парсер VCD (IEEE 1364). Читает $scope/$var/$timescale и поток '#', наполняя
/// Builder. Поддерживает $dumpvars, алиасы кодов, векторы (b), real (r) и
/// строки (s).
///
/// Состояние разбора живёт внутри вызова, поэтому один и тот же объект можно
/// использовать повторно: readHeader() и следом read() в другой Builder дадут
/// корректный результат.
class WASAFE_VCD_API VcdReader final : public Reader {
public:
    explicit VcdReader(std::filesystem::path path);
    VcdReader(const VcdReader&) = delete;
    VcdReader(VcdReader&&) = delete;
    ~VcdReader() override = default;

    [[nodiscard]] std::string_view format() const override { return "vcd"; }
    void read(Builder& sink) override { parse(sink, false); }
    void readHeader(Builder& sink) override { parse(sink, true); }

    VcdReader& operator=(const VcdReader&) = delete;
    VcdReader& operator=(VcdReader&&) = delete;

private:
    void parse(Builder& sink, bool headerOnly);

private:
    std::filesystem::path path_;
};

}  // namespace WaSafe::Vcd
