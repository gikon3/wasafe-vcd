#include "var_registry.hpp"

#include <algorithm>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <wasafe/types/type.hpp>
#include <wasafe/types/value_view.hpp>

namespace WaSafe::Vcd {

namespace {

/// Стартовый размер таблицы кодов: типичный дамп укладывается без роста, а
/// цена промаха — десятки килобайт на разбор.
constexpr std::size_t kExpectedVars = 1024;

/// Насколько шире наблюдённого набора индексов позволено быть массиву, прежде
/// чем группа признаётся разрежённой и не объединяется.
constexpr std::uint64_t kSparseFactor = 16;
constexpr std::uint64_t kSparseSlack = 64;

/// «Записи уровня не принадлежит ни одной группе» — обычный $var.
constexpr std::uint32_t kNoGroup = 0xFFFF'FFFFU;

}  // namespace

VarRegistry::VarRegistry(Builder& sink) : sink_{sink}, levels_(1) {
    codes_.reserve(kExpectedVars);
}

void VarRegistry::add(VarType vt, std::uint32_t size, std::string_view code, std::string_view ref) {
    const Reference r = parseReference(ref, indexScratch_);

    Level& level = levels_.back();
    PendingVar var;
    var.refPos = static_cast<std::uint32_t>(level.text.size());
    var.refLen = static_cast<std::uint32_t>(ref.size());
    level.text += ref;
    var.codePos = static_cast<std::uint32_t>(level.text.size());
    var.codeLen = static_cast<std::uint32_t>(code.size());
    level.text += code;
    var.indexPos = static_cast<std::uint32_t>(level.indices.size());
    var.indexCount = static_cast<std::uint32_t>(indexScratch_.size());
    level.indices.insert(level.indices.end(), indexScratch_.begin(), indexScratch_.end());
    var.baseLen = static_cast<std::uint32_t>(r.name.size());
    var.litLen = static_cast<std::uint32_t>(r.literal.size());
    var.leaf = leafSpec(vt, size, r);
    level.vars.push_back(var);
}

void VarRegistry::pushLevel() {
    levels_.emplace_back();
}

void VarRegistry::popLevel() {
    flushLevel();
    if (levels_.size() > 1)
        levels_.pop_back();
}

void VarRegistry::declarePlain(const Level& level, const PendingVar& var, std::string_view name) {
    ValueKind kind = ValueKind::LOGIC;
    const Type type = makeLeafType(var.leaf, kind);
    const std::string_view code = level.code(var);

    if (const VarInfo* existing = codes_.find(code)) {
        // Уже виденный код — алиас: новый узел иерархии делит существующий поток.
        std::ignore = sink_.declareVar(name, type, existing->id);
        return;
    }
    const SignalId id = sink_.declareVar(name, type);
    codes_.add(code, VarInfo{.id = id, .width = type->bitWidth(), .kind = kind});
}

void VarRegistry::prepareGroup(const Level& level, Group& group, const std::unordered_set<std::string_view>& plain) {
    const PendingVar& first = level.vars[group.members.front()];
    // Базовое имя, занятое обычным сигналом этого scope, шину не образует:
    // иначе в scope оказались бы два сигнала с одним именем.
    if (first.baseLen == 0 || plain.contains(level.base(first)))
        return;

    const std::span<const std::int32_t> firstIdx = level.idx(first);
    const std::size_t dims = firstIdx.size();
    group.leaf = first.leaf;

    // Границы измерений и направление (по первому и последнему появлению).
    std::vector<std::int32_t>& lo = dimLo_;
    std::vector<std::int32_t>& hi = dimHi_;
    std::vector<std::int32_t>& last = dimLast_;
    lo.assign(firstIdx.begin(), firstIdx.end());
    hi.assign(firstIdx.begin(), firstIdx.end());
    last.assign(firstIdx.begin(), firstIdx.end());

    // Однородность: одна размерность и один листовой тип на всю группу.
    std::unordered_set<std::string_view>& freshCodes = freshCodes_;
    freshCodes.clear();
    for (const std::size_t i : group.members) {
        const PendingVar& var = level.vars[i];
        const std::span<const std::int32_t> idx = level.idx(var);
        if (idx.size() != dims || !(var.leaf == group.leaf))
            return;
        // Один и тот же ещё не зарегистрированный код на двух элементах —
        // привязать их к общему потоку одним declareVar нечем, а потерять
        // данные элемента нельзя.
        const std::string_view code = level.code(var);
        if (codes_.find(code) == nullptr && !freshCodes.insert(code).second)
            return;
        for (std::size_t d = 0; d < dims; ++d) {
            lo[d] = std::min(lo[d], idx[d]);
            hi[d] = std::max(hi[d], idx[d]);
            last[d] = idx[d];
        }
    }

    group.dims.resize(dims);
    for (std::size_t d = 0; d < dims; ++d) {
        const bool ascending = firstIdx[d] <= last[d];
        group.dims[d] = ascending ? Dim{.left = lo[d], .right = hi[d]} : Dim{.left = hi[d], .right = lo[d]};
    }

    // Разрежённый набор индексов материализовал бы горы пустых потоков:
    // "$dumpvars top.mem[5]" не должен порождать миллион элементов.
    const std::uint64_t budget = kSparseFactor * group.members.size() + kSparseSlack;
    std::uint64_t total = 1;
    for (const Dim& dim : group.dims) {
        const std::uint64_t count = dim.count();
        if (count > budget || total > budget / count)
            return;
        total *= count;
    }

    // Два $var на один элемент — какой из них шина, неизвестно.
    group.ordinals.reserve(group.members.size());
    taken_.assign(total, 0);
    for (const std::size_t i : group.members) {
        const std::span<const std::int32_t> idx = level.idx(level.vars[i]);
        std::size_t ord = 0;
        for (std::size_t d = 0; d < dims; ++d)
            ord = ord * group.dims[d].count() + group.dims[d].ordinalOf(idx[d]);
        if (taken_[ord] != 0) {
            group.ordinals.clear();
            return;
        }
        taken_[ord] = 1;
        group.ordinals.push_back(ord);
    }

    group.merge = true;
}

void VarRegistry::materializeGroup(const Level& level, const Group& group) {
    ValueKind kind = ValueKind::LOGIC;
    const Type leafType = makeLeafType(group.leaf, kind);

    // Массивы всегда unpacked: у каждого элемента собственный поток, а packed
    // означал бы битовые срезы ОДНОГО потока, куда поэлементно не записать.
    Type type = leafType;
    for (std::size_t d = group.dims.size(); d-- > 0;)
        type = makeArray(type, group.dims[d].left, group.dims[d].right);

    // Код, уже привязанный к потоку, отдаётся как есть — это алиас на уровне
    // элемента; остальные потоки выделит builder и вернёт в этом же векторе.
    std::vector<SignalId> leaves(expansionStreamCount(type));
    for (std::size_t k = 0; k < group.members.size(); ++k) {
        if (const VarInfo* existing = codes_.find(level.code(level.vars[group.members[k]])))
            leaves[group.ordinals[k]] = existing->id;
    }

    std::ignore = sink_.declareVar(level.base(level.vars[group.members.front()]), type, std::nullopt, leaves);

    for (std::size_t k = 0; k < group.members.size(); ++k) {
        codes_.add(level.code(level.vars[group.members[k]]),
                VarInfo{.id = leaves[group.ordinals[k]], .width = leafType->bitWidth(), .kind = kind});
    }
}

void VarRegistry::flushLevel() {
    Level& level = levels_.back();
    if (level.vars.empty())
        return;

    // Группа для каждой записи уровня; kNoGroup — обычный $var. Индекс держим
    // рядом, чтобы во втором проходе не искать группу по имени заново.
    std::vector<std::uint32_t> groupOfVar(level.vars.size(), kNoGroup);
    // Ключи — виды на арену уровня: она уже не растёт, копировать имена незачем.
    std::unordered_map<std::string_view, std::uint32_t> groupIndex;
    std::vector<Group> groups;

    for (std::size_t i = 0; i < level.vars.size(); ++i) {
        const PendingVar& var = level.vars[i];
        if (var.indexCount == 0)
            continue;
        const auto [it, inserted] = groupIndex.try_emplace(level.base(var), static_cast<std::uint32_t>(groups.size()));
        if (inserted)
            groups.emplace_back();
        groups[it->second].members.push_back(i);
        groupOfVar[i] = it->second;
    }

    // Имена обычных $var нужны только для проверки конфликта с базовым именем
    // шины, поэтому на уровне без индексов ни прохода, ни множества нет: в
    // типичном дампе это весь заголовок.
    if (!groups.empty()) {
        std::unordered_set<std::string_view> plain;
        for (const PendingVar& var : level.vars) {
            if (var.indexCount == 0)
                plain.insert(level.literal(var));
        }
        for (Group& group : groups)
            prepareGroup(level, group, plain);
    }

    // Порядок объявления сохраняется: шина занимает место своего первого элемента.
    for (std::size_t i = 0; i < level.vars.size(); ++i) {
        const PendingVar& var = level.vars[i];
        if (groupOfVar[i] == kNoGroup) {
            declarePlain(level, var, level.literal(var));
            continue;
        }
        Group& group = groups[groupOfVar[i]];
        if (!group.merge) {
            declarePlain(level, var, level.literal(var));  // группа не сложилась — литеральное имя
        }
        else if (!group.done) {
            materializeGroup(level, group);
            group.done = true;
        }
    }

    level.clear();
}

}  // namespace WaSafe::Vcd
