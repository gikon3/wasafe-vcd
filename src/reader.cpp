#include "wasafe_vcd/reader.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include "scanner.hpp"
#include "value_buffer.hpp"
#include "var_registry.hpp"
#include "vcd_syntax.hpp"
#include "wasafe_vcd/exception.hpp"

namespace WaSafe::Vcd {

namespace {

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
/// Разбор одного файла в один Builder: главный цикл, директивы заголовка и
/// поток изменений значений. Создаётся на каждый вызов read()/readHeader(),
/// поэтому состояние (таблица кодов, буферы) заведомо чистое.
///
/// Всё, что не относится к самому обходу токенов, вынесено наружу: лексика
/// формата — в vcd_syntax, отложенные объявления $var — в VarRegistry,
/// бит-планы значений — в ValueBuffer.
class Parser final {
public:
    Parser(const std::filesystem::path& path, Builder& sink) : scan_{path}, sink_{sink}, vars_{sink} {}

    void run(bool headerOnly);

private:
    /// @return true, если это было $enddefinitions.
    bool directive(std::string_view tok);
    void skipToEnd();
    void finishHeader();

    void parseScope();
    void parseTimescaleCmd();
    void parseVar();
    void parseTime(std::string_view tok);
    void parseValueChange(std::string_view tok);

    /// Закрыть текущий scope: сначала объявить отложенные $var, потом выйти.
    void closeScope();

private:
    Scanner scan_;
    Builder& sink_;
    VarRegistry vars_;
    ValueBuffer value_;

    // Scratch под токены, которые должны пережить следующий scan_.next().
    std::string text_;
    std::string ref_;
    std::string code_;

    bool headerDone_ = false;
    bool timeSet_ = false;
};

void Parser::run(bool headerOnly) {
    while (true) {
        const std::string_view tok = scan_.next();
        if (tok.empty())
            break;

        if (tok.front() == '$') {
            if (directive(tok)) {  // $enddefinitions
                finishHeader();
                if (headerOnly)
                    return;
            }
        }
        else if (tok.front() == '#') {
            finishHeader();  // на случай отсутствия $enddefinitions
            parseTime(tok);
        }
        else {
            finishHeader();
            if (!timeSet_) {
                sink_.setTime(0);
                timeSet_ = true;
            }
            parseValueChange(tok);
        }
    }
    finishHeader();
}

bool Parser::directive(std::string_view tok) {
    // Отбор по второму символу: полное сравнение со всеми ключевыми словами
    // заметно дороже, а $-токенов в дампе с частыми $dumpoff/$dumpon много.
    switch (tok.size() < 2 ? '\0' : tok[1]) {
        case 'v':
            if (tok == "$var") {
                parseVar();
                return false;
            }
            break;  // $version — обычная секция до $end
        case 's':
            if (tok == "$scope") {
                parseScope();
                return false;
            }
            break;
        case 'u':
            if (tok == "$upscope") {
                skipToEnd();
                closeScope();
                return false;
            }
            break;
        case 't':
            if (tok == "$timescale") {
                parseTimescaleCmd();
                return false;
            }
            break;
        case 'e':
            if (tok == kEnd)  // закрывает прозрачную секцию дампа
                return false;
            if (tok == "$enddefinitions") {
                skipToEnd();
                return true;
            }
            break;
        case 'd':
            if (isDumpKeyword(tok))
                return false;
            break;  // $date
        default:
            break;
    }
    skipToEnd();  // $date / $version / $comment / неизвестное — до $end
    return false;
}

void Parser::skipToEnd() {
    while (true) {
        const std::string_view t = scan_.next();
        if (t.empty() || t == kEnd)
            return;
    }
}

void Parser::finishHeader() {
    if (headerDone_)
        return;
    // Незакрытые $scope (файл без $upscope) сбрасываются изнутри наружу: иначе
    // отложенные $var внешнего уровня попали бы во вложенный scope.
    while (vars_.depth() > 1)
        closeScope();
    vars_.flushLevel();
    sink_.headerDone();
    headerDone_ = true;
}

void Parser::closeScope() {
    vars_.popLevel();
    sink_.endScope();
}

void Parser::parseScope() {
    std::string_view tok = scan_.next();
    if (tok.empty() || tok == kEnd)
        throw Error{"truncated $scope"};
    const ScopeKind kind = scopeKind(tok);

    tok = scan_.next();
    if (tok.empty() || tok == kEnd)
        throw Error{"$scope without name"};
    text_.assign(tok);  // имя должно пережить skipToEnd()

    skipToEnd();
    sink_.beginScope(text_, kind);
    vars_.pushLevel();
}

void Parser::parseTimescaleCmd() {
    text_.clear();
    while (true) {
        const std::string_view t = scan_.next();
        if (t.empty() || t == kEnd)
            break;
        text_ += t;
    }
    if (const auto ts = parseTimescale(text_))
        sink_.setTimeScale(*ts);
}

void Parser::parseVar() {
    std::string_view tok = scan_.next();
    if (tok.empty() || tok == kEnd)
        throw Error{"truncated $var"};
    const VarType vt = varType(tok);

    tok = scan_.next();
    if (tok.empty() || tok == kEnd)
        throw Error{"$var without size"};
    std::uint32_t size = 0;
    if (!parseNumber(tok, size))
        throw Error{"bad $var size '" + std::string{tok} + "'"};

    tok = scan_.next();
    if (tok.empty() || tok == kEnd)
        throw Error{"$var without identifier code"};
    code_.assign(tok);

    ref_.clear();
    while (true) {
        tok = scan_.next();
        if (tok.empty() || tok == kEnd)
            break;
        ref_ += tok;
    }
    if (ref_.empty())
        throw Error{"$var without reference"};

    // Объявление откладывается до конца scope: пока не виден весь уровень,
    // неизвестны ни границы измерений шины, ни конфликты имён.
    vars_.add(vt, size, code_, ref_);
}

void Parser::parseTime(std::string_view tok) {
    TimeStamp t = 0;
    if (!parseNumber(tok.substr(1), t))
        throw Error{"bad timestamp '" + std::string{tok} + "'"};
    sink_.setTime(t);
    timeSet_ = true;
}

void Parser::parseValueChange(std::string_view tok) {
    switch (tok.front()) {
        case '0':
        case '1':
        case 'x':
        case 'X':
        case 'z':
        case 'Z': {
            // Скаляр: код лежит в том же токене.
            const VarInfo* var = vars_.find(tok.substr(1));
            if (var == nullptr || var->kind != ValueKind::LOGIC)
                return;
            if (var->width == 1) {  // самый частый вид изменения в дампе
                sink_.valueChange(var->id, singleBitView(logicFromChar(tok.front())));
                return;
            }
            // Скаляр на широком сигнале расширяется по тому же правилу, что и
            // вектор: 'x' на 8 битах даёт "xxxxxxxx".
            value_.decode(tok.substr(0, 1));
            sink_.valueChange(var->id, value_.view(var->width, extensionLogic(tok.front())));
            break;
        }
        case 'b':
        case 'B': {
            // Значение разбирается ДО чтения кода: следующий next() двигает
            // буфер сканера и обесценивает tok.
            const std::string_view bits = tok.substr(1);
            const Logic ext = extensionLogic(bits.empty() ? '0' : bits.front());
            value_.decode(bits);
            const VarInfo* var = vars_.find(scan_.next());
            if (var == nullptr || var->kind != ValueKind::LOGIC)
                return;
            sink_.valueChange(var->id, value_.view(var->width, ext));
            break;
        }
        case 'r':
        case 'R': {
            double value = 0;
            const bool ok = parseNumber(tok.substr(1), value);
            const VarInfo* var = vars_.find(scan_.next());  // код читаем всегда
            if (!ok || var == nullptr || var->kind != ValueKind::REAL)
                return;
            sink_.valueChange(var->id, value);
            break;
        }
        case 's':
        case 'S': {
            text_.assign(tok.substr(1));
            const VarInfo* var = vars_.find(scan_.next());
            if (var == nullptr || var->kind != ValueKind::STRING)
                return;
            sink_.valueChange(var->id, std::string_view{text_});
            break;
        }
        default:
            break;  // неизвестная форма — пропускаем
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// VcdReader
// ---------------------------------------------------------------------------
VcdReader::VcdReader(std::filesystem::path path) : path_{std::move(path)} {
}

void VcdReader::parse(Builder& sink, bool headerOnly) {
    Parser parser{path_, sink};
    parser.run(headerOnly);
}

}  // namespace WaSafe::Vcd
