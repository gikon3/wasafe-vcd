#include "wasafe_vcd/writer.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <wasafe/core/time.hpp>
#include <wasafe/model/database.hpp>

#include "code_book.hpp"
#include "emitter.hpp"
#include "value_text.hpp"
#include "vcd_syntax.hpp"
#include "wasafe_vcd/version.hpp"

namespace WaSafe::Vcd {

namespace {

// ---------------------------------------------------------------------------
// Dumper
// ---------------------------------------------------------------------------
/// Выгрузка одной Database в один файл: заголовок, иерархия и поток значений.
/// Создаётся на каждый вызов write(), поэтому состояние заведомо чистое.
class Dumper final {
public:
    Dumper(const Database& db, const std::filesystem::path& path);

    void run();

private:
    void writeHeader();

    /// Содержимое scope: сначала $var своих сигналов, затем вложенные scope.
    void writeScopeBody(const Scope& scope);
    /// $scope ... $upscope вокруг writeScopeBody().
    void writeScope(const Scope& scope);
    /// Узел сигнала: $var, если у него свой поток, иначе спуск в children().
    void declareSignal(const Signal& signal);
    void writeVar(const TypeDescriptor& type, SignalId id);

    void writeValues();
    void writeTime(TimeStamp t);
    void writeChange(std::uint32_t ordinal, const ValueView& value);
    void writeScalar(std::string_view code, LogicVectorView v);
    void writeVector(std::string_view code, LogicVectorView v);
    void writeReal(std::string_view code, double v);
    void writeString(std::string_view code, std::string_view v);

    void appendStream(std::uint32_t ordinal, SignalId id, std::uint32_t size);

private:
    const Database& db_;
    Emitter out_;
    CodeBook codes_;

    // Параллельные массивы на уникальные потоки в порядке первого появления;
    // индекс — порядковый номер потока, он же source курсора.
    std::vector<SignalId> streams_;
    std::vector<std::uint32_t> widths_;

    /// Ссылка текущего узла: локальные имена от верхнего уровня scope вниз.
    /// Растёт и откатывается по ходу обхода, аллокаций на узел нет.
    std::string ref_;
};

Dumper::Dumper(const Database& db, const std::filesystem::path& path) : db_{db}, out_{path} {
}

void Dumper::run() {
    writeHeader();
    writeScopeBody(db_.root());
    out_.text("$enddefinitions $end\n");
    writeValues();
    out_.finish();
}

void Dumper::writeHeader() {
    out_.text(std::format("$version wasafe-vcd {} $end\n", Version::kString));
    out_.text("$timescale " + formatTimeScale(db_.timeScale()) + " $end\n");
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева scope по определению рекурсивен
void Dumper::writeScopeBody(const Scope& scope) {
    for (auto&& signal : scope.signals()) {
        assert(ref_.empty());
        declareSignal(signal);
    }
    for (auto&& child : scope.scopes())
        writeScope(child);
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева scope по определению рекурсивен
void Dumper::writeScope(const Scope& scope) {
    out_.text(std::format("$scope {} {} $end\n", scopeKeyword(scope.kind()), scope.name()));
    writeScopeBody(scope);
    out_.text("$upscope $end\n");
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева композита по определению рекурсивен
void Dumper::declareSignal(const Signal& signal) {
    const auto mark = ref_.size();
    appendReferencePart(ref_, signal.name());
    if (const auto id = signal.streamId()) {
        const auto type = signal.type();
        appendRange(ref_, *type);
        writeVar(*type, *id);
    }
    else {
        for (auto&& child : signal.children())
            declareSignal(child);
    }
    ref_.resize(mark);
}

void Dumper::writeVar(const TypeDescriptor& type, SignalId id) {
    const auto a = codes_.assign(id);
    const auto size = varSize(type);
    if (a.fresh)
        appendStream(a.ordinal, id, size);
    out_.text(std::format("$var {} {} {} {} $end\n", varKeyword(type), size, a.code, ref_));
}

void Dumper::writeValues() {
    if (streams_.empty())
        return;

    const auto cursor = db_.storage().openCursor(streams_, kWholeTime);
    bool haveTime = false;
    TimeStamp now = kNoTime;

    while (cursor->next()) {
        const auto c = cursor->current();
        if (!haveTime || c.time != now) {
            writeTime(c.time);
            now = c.time;
            haveTime = true;
        }
        writeChange(c.source, c.value);
    }
}

void Dumper::writeTime(TimeStamp t) {
    auto* p = out_.reserve(1 + kMaxTimeChars + 1);
    *p++ = '#';
    p = formatTime(p, t);
    *p++ = '\n';
    out_.commit(p);
}

void Dumper::writeChange(std::uint32_t ordinal, const ValueView& value) {
    const auto code = codes_.code(ordinal);
    switch (value.kind()) {
        case ValueKind::LOGIC:
            widths_[ordinal] == 1 ? writeScalar(code, value.logic()) : writeVector(code, value.logic());
            break;
        case ValueKind::REAL:
            writeReal(code, value.real());
            break;
        case ValueKind::STRING:
            writeString(code, value.string());
            break;
        default:
            break;
    }
}

void Dumper::writeScalar(std::string_view code, LogicVectorView v) {
    auto* p = out_.reserve(1 + code.size() + 1);  //< '<0|1|x|z><код>\n'
    *p++ = scalarChar(v);
    std::memcpy(p, code.data(), code.size());
    p += code.size();
    *p++ = '\n';
    out_.commit(p);
}

void Dumper::writeVector(std::string_view code, LogicVectorView v) {
    auto* p = out_.reserve(1 + v.width() + 1 + code.size() + 1);  //< 'b<биты> <код>\n'
    *p++ = 'b';
    auto* end = encodeLogic(p, v);
    const auto* keep = trimmedBegin(p, end);
    if (keep != p) {
        const std::size_t sz = end - keep;
        std::memmove(p, keep, sz);
        end = p + sz;
    }
    *end++ = ' ';
    std::memcpy(end, code.data(), code.size());
    end += code.size();
    *end++ = '\n';
    out_.commit(end);
}

void Dumper::writeReal(std::string_view code, double v) {
    auto* p = out_.reserve(1 + kMaxRealChars + 1 + code.size() + 1);  //< 'r<число> <код>\n'
    *p++ = 'r';
    p = formatReal(p, v);
    *p++ = ' ';
    std::memcpy(p, code.data(), code.size());
    p += code.size();
    *p++ = '\n';
    out_.commit(p);
}

void Dumper::writeString(std::string_view code, std::string_view v) {
    auto* p = out_.reserve(1 + v.size() + 1 + code.size() + 1);  //< 's<текст> <код>\n'
    *p++ = 's';
    std::memcpy(p, v.data(), v.size());
    p += v.size();
    *p++ = ' ';
    std::memcpy(p, code.data(), code.size());
    p += code.size();
    *p++ = '\n';
    out_.commit(p);
}

void Dumper::appendStream([[maybe_unused]] std::uint32_t ordinal, SignalId id, std::uint32_t size) {
    // Проверка, что номера ordinal идут монотонно без пропусков.
    assert(ordinal == streams_.size());
    streams_.push_back(id);
    widths_.push_back(size);
}

}  // namespace

void VcdWriter::write(const Database& db, const std::filesystem::path& path) {
    Dumper{db, path}.run();
}

}  // namespace WaSafe::Vcd
