#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace WaSafe::Vcd {

/// Потоковый вывод текстового VCD: собственный буфер поверх ofstream без его
/// буферизации.
class Emitter final {
public:
    explicit Emitter(std::filesystem::path path, std::size_t bufferSize = kDefaultBufferSize);
    Emitter(const Emitter&) = delete;
    Emitter(Emitter&&) = delete;
    /// Закрывает и удаляет временный файл. Успешный путь всегда идёт через finish().
    ~Emitter();

    /// n гарантированно свободных байт. Быстрый путь инлайнится намеренно.
    [[nodiscard]] char* reserve(std::size_t n) {
        if (pos_ + n > buf_.size())
            flushFor(n);
        return buf_.data() + pos_;
    }

    /// Подтвердить фактически записанное; end — конец заполненного участка.
    void commit(const char* end) { pos_ = static_cast<std::size_t>(end - buf_.data()); }

    /// Обёртка для записи (без горячего пути).
    void text(std::string_view s);

    /// Дописать остаток и закрыть файл. Атомарно заменяет целевой файл.
    /// Единственное место, где ошибка записи превращается в исключение на успешном пути.
    void finish();

    Emitter& operator=(const Emitter&) = delete;
    Emitter& operator=(Emitter&&) = delete;

public:
    static constexpr std::size_t kDefaultBufferSize = std::size_t{1} << 20U;

private:
    /// Сбросить буфер в файл; если n не влезает даже в пустой — вырастить его.
    void flushFor(std::size_t n);

private:
    std::filesystem::path path_;
    std::filesystem::path tmpPath_;
    std::ofstream out_;
    std::vector<char> buf_;
    std::size_t pos_ = 0;
    bool committed_ = false;
};

}  // namespace WaSafe::Vcd
