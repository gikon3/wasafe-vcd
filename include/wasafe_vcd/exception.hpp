#pragma once

#include <string>
#include <wasafe/core/exception.hpp>

#include "wasafe_vcd/export.hpp"

namespace WaSafe::Vcd {

/// Ошибка разбора или записи VCD.
class WASAFE_VCD_API Error : public Exception {
public:
    explicit Error(const std::string& msg) : Exception{"vcd: " + msg} {}
};

}  // namespace WaSafe::Vcd
