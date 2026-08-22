#include "vcd_syntax.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <iterator>

namespace WaSafe::Vcd {

ScopeKind scopeKind(std::string_view t) {
    if (t == "module")
        return ScopeKind::MODULE;
    if (t == "task")
        return ScopeKind::TASK;
    if (t == "function")
        return ScopeKind::FUNCTION;
    if (t == "begin" || t == "fork")
        return ScopeKind::BLOCK;
    return ScopeKind::UNKNOWN;
}

VarType varType(std::string_view t) {
    if (t == "real" || t == "realtime")
        return VarType::REAL;
    if (t == "shortreal")
        return VarType::SHORT_REAL;
    if (t == "string")
        return VarType::STRING;
    return VarType::LOGIC;
}

bool isDumpKeyword(std::string_view tok) {
    return tok == "$dumpvars" || tok == "$dumpall" || tok == "$dumpon" || tok == "$dumpoff" || tok == "$dumpports";
}

namespace {

/// Десятичная приставка единицы времени -> показатель степени десяти.
std::optional<std::int32_t> unitExponent(std::string_view u) {
    if (u == "s")
        return 0;
    if (u == "ms")
        return -3;
    if (u == "us")
        return -6;
    if (u == "ns")
        return -9;
    if (u == "ps")
        return -12;
    if (u == "fs")
        return -15;
    return std::nullopt;
}

}  // namespace

std::optional<TimeScale> parseTimescale(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] >= '0' && s[i] <= '9'))
        ++i;
    if (i == 0)
        return std::nullopt;
    std::int64_t scale = 0;
    if (!parseNumber(s.substr(0, i), scale))
        return std::nullopt;
    const auto exp = unitExponent(s.substr(i));
    if (!exp)
        return std::nullopt;
    return TimeScale{.exponent = *exp, .scale = scale};
}

Reference parseReference(std::string_view ref, std::vector<std::int32_t>& indices) {
    indices.clear();
    Reference r;
    std::string_view rest = ref;

    while (rest.size() >= 2 && rest.back() == ']') {
        const std::size_t open = rest.rfind('[');
        if (open == std::string_view::npos)
            break;
        const std::string_view inner = rest.substr(open + 1, rest.size() - open - 2);

        if (const std::size_t colon = inner.find(':'); colon != std::string_view::npos) {
            // Диапазон допустим один и только справа: левее индексов он уже
            // описывает не разряды сигнала, а что-то в имени.
            if (r.hasRange || !indices.empty())
                break;
            if (!parseNumber(inner.substr(0, colon), r.msb) || !parseNumber(inner.substr(colon + 1), r.lsb))
                break;
            r.hasRange = true;
            r.literal = ref.substr(0, open);  // диапазон в имя не входит
        }
        else {
            std::int32_t index = 0;
            if (!parseNumber(inner, index))
                break;
            indices.push_back(index);
        }
        rest = rest.substr(0, open);
    }

    if (rest.empty()) {  // от имени ничего не осталось — значит скобки были им
        indices.clear();
        r = Reference{};
        rest = ref;
    }
    if (!r.hasRange)
        r.literal = ref;
    std::ranges::reverse(indices);
    r.name = rest;
    return r;
}

LeafSpec leafSpec(VarType vt, std::uint32_t size, const Reference& r) {
    LeafSpec s;
    s.vt = vt;
    if (vt != VarType::LOGIC)
        return s;

    s.width = size != 0 ? size : 1U;
    // Диапазону доверяем, только если он сходится с size: иначе тип и поток
    // значений разъехались бы по ширине.
    const bool rangeFits = r.hasRange && static_cast<std::uint32_t>(std::abs(r.msb - r.lsb) + 1) == s.width;
    if (s.width == 1)
        return s;  // скаляр: границы не важны и нормализованы, чтобы specs сравнивались
    s.msb = rangeFits ? r.msb : static_cast<std::int32_t>(s.width) - 1;
    s.lsb = rangeFits ? r.lsb : 0;
    return s;
}

Type makeLeafType(const LeafSpec& s, ValueKind& kind) {
    switch (s.vt) {
        case VarType::REAL:
        case VarType::SHORT_REAL:
            kind = ValueKind::REAL;
            return makeReal(s.vt == VarType::SHORT_REAL);
        case VarType::STRING:
            kind = ValueKind::STRING;
            return makeString();
        case VarType::LOGIC:
            break;
    }
    kind = ValueKind::LOGIC;
    return s.width == 1 ? makeScalar() : makeVector(s.msb, s.lsb);
}

std::string_view scopeKeyword(ScopeKind k) {
    switch (k) {
        case ScopeKind::TASK:
            return "task";
        case ScopeKind::FUNCTION:
            return "function";
        case ScopeKind::BLOCK:
            return "begin";
        default:
            return "module";
    }
}

std::string_view varKeyword(const TypeDescriptor& t) {
    switch (t.kind()) {
        case TypeKind::REAL: {
            const auto* real = t.as<RealType>();
            return real != nullptr && real->isShortreal() ? "shortreal" : "real";
        }
        case TypeKind::STRING:
            return "string";
        case TypeKind::EVENT:
            return "event";
        default:
            return "wire";
    }
}

std::uint32_t varSize(const TypeDescriptor& t) {
    switch (t.kind()) {
        case TypeKind::REAL: {
            const auto* real = t.as<RealType>();
            return real != nullptr && real->isShortreal() ? 32 : 64;
        }
        case TypeKind::STRING:
        case TypeKind::EVENT:
            return 1;
        default:
            return t.bitWidth();
    }
}

void appendReferencePart(std::string& ref, std::string_view part) {
    if (part.empty())
        return;

    if (ref.empty()) {
        ref = part;
    }
    else if (part.front() == '[') {
        ref += part;
    }
    else {
        ref += '.';
        ref += part;
    }
}

void appendRange(std::string& ref, const TypeDescriptor& t) {
    const TypeDescriptor* src = &t;
    if (const auto* enumeration = t.as<EnumType>())
        src = enumeration->base().get();
    const auto* vec = src != nullptr ? src->as<VectorType>() : nullptr;
    if (vec == nullptr || vec->bitWidth() <= 1)
        return;

    std::array<char, 32> buf{};  // " [" + int32 + ':' + int32 + ']' + align
    char* p = buf.data();

    *p++ = ' ';
    *p++ = '[';
    p = std::to_chars(p, std::end(buf), vec->msb()).ptr;
    *p++ = ':';
    p = std::to_chars(p, std::end(buf), vec->lsb()).ptr;
    *p++ = ']';

    ref.append(buf.data(), p);
}

}  // namespace WaSafe::Vcd
