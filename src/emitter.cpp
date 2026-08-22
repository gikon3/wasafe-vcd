#include "emitter.hpp"

#include <algorithm>
#include <format>
#include <system_error>

#include "wasafe_vcd/exception.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace WaSafe::Vcd {

namespace {

#ifdef _WIN32
using PidType = DWORD;
PidType getPid() {
    return GetCurrentProcessId();
}
#else
using PidType = pid_t;
PidType getPid() {
    return getpid();
}
#endif

std::filesystem::path tempPathFor(const std::filesystem::path& path) {
    auto result = path;
    result += std::format(".{}.tmp", getPid());
    return result;
}

}  // namespace

Emitter::Emitter(std::filesystem::path path, std::size_t bufferSize) :
        path_{std::move(path)}, tmpPath_{tempPathFor(path_)}, buf_(bufferSize) {
    out_.rdbuf()->pubsetbuf(nullptr, 0);
    out_.open(tmpPath_, std::ios::binary | std::ios::trunc);
    if (!out_)
        throw Error{"cannot open " + path_.string()};
}

Emitter::~Emitter() {
    if (committed_)
        return;

    // finish() не вызван: временный файл удаляется.
    out_.close();
    std::error_code ec;
    std::filesystem::remove(tmpPath_, ec);
}

void Emitter::text(std::string_view s) {
    char* const dst = reserve(s.size());
    commit(std::ranges::copy(s, dst).out);
}

void Emitter::finish() {
    if (committed_)
        return;

    flushFor(0);
    out_.close();
    if (!out_) [[unlikely]]
        throw Error{"write failed: " + path_.string()};

    std::error_code ec;
    std::filesystem::rename(tmpPath_, path_, ec);
    if (ec) [[unlikely]]
        throw Error{"cannot replace " + path_.string() + ": " + ec.message()};
    committed_ = true;
}

void Emitter::flushFor(std::size_t n) {
    if (pos_ > 0) {
        out_.write(buf_.data(), static_cast<std::streamsize>(pos_));
        if (!out_) [[unlikely]]
            throw Error{"write failed: " + path_.string()};
        pos_ = 0;
    }

    if (n > buf_.size())
        buf_.resize(n);
}

}  // namespace WaSafe::Vcd
