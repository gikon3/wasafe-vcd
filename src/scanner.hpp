#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace WaSafe::Vcd {

/// Потоковый токенайзер текстового VCD: читает файл крупными блоками в
/// собственный буфер и отдаёт токены как string_view внутрь этого буфера.
/// Заменяет `istream >> std::string`, у которого на каждый токен приходится
/// sentry, locale-aware isspace и аллокация строки.
///
/// ВАЖНО: возвращённый токен валиден только до следующего вызова next() — при
/// дозаливке содержимое буфера сдвигается. Всё, что нужно пережить следующий
/// токен, вызывающая сторона обязана скопировать или разобрать сразу.
///
/// Разделителями считаются все байты <= ' ' (пробел, табуляция, CR, LF), так
/// что дампы с CRLF читаются без отдельной обработки.
class Scanner final {
public:
    explicit Scanner(const std::filesystem::path& path, std::size_t bufferSize = kDefaultBufferSize);
    Scanner(const Scanner&) = delete;
    Scanner(Scanner&&) = delete;
    ~Scanner() = default;

    /// Следующий токен; пустой view означает конец файла.
    ///
    /// Быстрый путь (токен целиком лежит в буфере) намеренно инлайнится: на
    /// большом дампе это десятки миллионов вызовов. Всё, что связано с
    /// границей блока, вынесено в nextSlow().
    [[nodiscard]] std::string_view next() {
        const char* const base = buf_.data();
        const char* p = base + pos_;
        const char* const last = base + end_;

        while (p != last && isSpace(*p))
            ++p;
        const char* const start = p;
        while (p != last && !isSpace(*p))
            ++p;

        if (p == last) {  // токен может продолжаться в следующем блоке
            pos_ = static_cast<std::size_t>(start - base);
            return nextSlow();
        }
        pos_ = static_cast<std::size_t>(p - base);
        return {start, static_cast<std::size_t>(p - start)};
    }

    Scanner& operator=(const Scanner&) = delete;
    Scanner& operator=(Scanner&&) = delete;

public:
    static constexpr std::size_t kDefaultBufferSize = std::size_t{1} << 20U;

private:
    /// Разделитель токенов: любой управляющий символ или пробел.
    [[nodiscard]] static constexpr bool isSpace(char c) { return static_cast<unsigned char>(c) <= ' '; }

    /// Путь через границу блока: дочитывает файл столько раз, сколько нужно.
    [[nodiscard]] std::string_view nextSlow();

    /// Сдвинуть хвост [keepFrom, end) в начало буфера и дочитать файл.
    /// После вызова сохранённый хвост всегда лежит с нулевого смещения.
    /// @return false, если файл кончился и новых данных не появилось.
    bool refill(std::size_t keepFrom);

private:
    std::ifstream in_;
    std::vector<char> buf_;
    std::size_t pos_ = 0;  ///< начало неразобранного текста в буфере
    std::size_t end_ = 0;  ///< конец валидных данных в буфере
    bool eof_ = false;
};

}  // namespace WaSafe::Vcd
