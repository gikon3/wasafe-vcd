#include <cstdio>
#include <filesystem>
#include <wasafe_vcd/config.hpp>
#include <wasafe_vcd/export.hpp>
#include <wasafe_vcd/reader.hpp>
#include <wasafe_vcd/version.hpp>
#include <wasafe_vcd/writer.hpp>

int main() {
    const WaSafe::Vcd::VcdReader reader{std::filesystem::path{"nonexistent.vcd"}};

    std::printf("wasafe-vcd %s, format: %s, %s\n", WaSafe::Vcd::Version::kString.data(), reader.format().data(),
            WaSafe::Vcd::kDescription.data());
    return 0;
}
