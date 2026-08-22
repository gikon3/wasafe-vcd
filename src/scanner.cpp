#include "scanner.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>

#include "wasafe_vcd/exception.hpp"

namespace WaSafe::Vcd {

namespace {

/// Буфер под размер файла: маленькие дампы не должны платить за мегабайт.
std::size_t bufferFor(const std::filesystem::path& path, std::size_t requested) {
    const std::size_t want = requested == 0 ? Scanner::kDefaultBufferSize : requested;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0)
        return want;
    return std::min<std::size_t>(want, static_cast<std::size_t>(size));
}

}  // namespace

Scanner::Scanner(const std::filesystem::path& path, std::size_t bufferSize) : buf_(bufferFor(path, bufferSize)) {
    in_.rdbuf()->pubsetbuf(nullptr, 0);
    in_.open(path, std::ios::binary);
    if (!in_)
        throw Error{"cannot open " + path.string()};
}

std::string_view Scanner::nextSlow() {
    // 1. Пропустить разделители, дозаливая буфер по мере надобности.
    while (true) {
        while (pos_ < end_ && isSpace(buf_[pos_]))
            ++pos_;
        if (pos_ < end_)
            break;
        if (!refill(pos_))
            return {};
    }

    // 2. Дочитать токен; на границе блока хвост переезжает в начало буфера.
    std::size_t start = pos_;
    while (true) {
        while (pos_ < end_ && !isSpace(buf_[pos_]))
            ++pos_;
        if (pos_ < end_)
            return {buf_.data() + start, pos_ - start};

        const std::size_t len = pos_ - start;
        if (!refill(start)) {  // файл кончился — токен уже целиком в буфере
            pos_ = len;
            return {buf_.data(), len};
        }
        start = 0;
        pos_ = len;
    }
}

bool Scanner::refill(std::size_t keepFrom) {
    const std::size_t tail = end_ - keepFrom;
    if (tail > 0 && keepFrom > 0)
        std::memmove(buf_.data(), buf_.data() + keepFrom, tail);
    if (tail == buf_.size())  // токен длиннее буфера — растим буфер
        buf_.resize(buf_.size() * 2);
    pos_ = tail;
    end_ = tail;

    if (eof_)
        return false;
    in_.read(buf_.data() + tail, static_cast<std::streamsize>(buf_.size() - tail));
    const auto got = static_cast<std::size_t>(in_.gcount());
    if (got == 0) {
        eof_ = true;
        return false;
    }
    end_ = tail + got;
    return true;
}

}  // namespace WaSafe::Vcd
