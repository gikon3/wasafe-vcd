#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <wasafe/io/builder.hpp>
#include <wasafe/io/ingest.hpp>
#include <wasafe/model/database.hpp>

#include "counting_builder.hpp"
#include "vcd_gen.hpp"
#include "wasafe_vcd/reader.hpp"
#include "wasafe_vcd/writer.hpp"

using namespace WaSafe;

namespace {

/// Размер синтетического дампа. Подобран так, чтобы пройти пути, недоступные
/// мелким тестам: коды сигналов выходят за один байт (сигналов больше 94), а на
/// каждый сигнал приходится больше 4096 изменений — значения blockChanges по
/// умолчанию, — поэтому потоки заведомо разбиваются на несколько блоков
/// хранилища. При этом весь набор укладывается в доли секунды и остаётся в
/// обычном прогоне; для замеров есть отдельный wasafe_vcd_bench.
constexpr std::uint64_t kDumpBytes = 24ULL << 20U;

/// Шаги, на которых сверяются значения: начало, середина и последний.
[[nodiscard]] std::vector<std::uint64_t> probeSteps(std::uint64_t steps) {
    return {0, 1, steps / 3, steps / 2, steps - 1};
}

/// Сверить значения БД с формулами генератора на шагах probeSteps().
void expectMatchesGenerator(const Database& db, std::uint64_t steps) {
    for (const std::uint64_t step : probeSteps(steps)) {
        const auto t = static_cast<TimeStamp>(step);
        SCOPED_TRACE("step " + std::to_string(step));

        for (std::uint32_t i = 0; i < VcdGen::kScalars; ++i) {
            const auto sig = db.find(VcdGen::scalarName(i));
            ASSERT_TRUE(sig) << VcdGen::scalarName(i);
            EXPECT_EQ(sig->valueAt(t).asLogic().toString(), std::string(1, VcdGen::scalarAt(i, step)))
                    << VcdGen::scalarName(i);
        }
        for (std::uint32_t i = 0; i < VcdGen::kVectors; ++i) {
            const auto sig = db.find(VcdGen::vectorName(i));
            ASSERT_TRUE(sig) << VcdGen::vectorName(i);
            EXPECT_EQ(sig->valueAt(t).asLogic().toString(), VcdGen::vectorString(i, step)) << VcdGen::vectorName(i);
        }
        for (std::uint32_t i = 0; i < VcdGen::kReals; ++i) {
            const auto sig = db.find(VcdGen::realName(i));
            ASSERT_TRUE(sig) << VcdGen::realName(i);
            EXPECT_DOUBLE_EQ(sig->valueAt(t).asReal(), VcdGen::realAt(i, step)) << VcdGen::realName(i);
        }
    }
}

/// Дамп генерируется один раз на весь набор — он одинаков для всех тестов.
class VcdLarge : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dump_.emplace("wasafe_large_" + std::to_string(std::random_device{}()) + ".vcd");
        steps_ = VcdGen::write(dump_->path, kDumpBytes);
    }
    static void TearDownTestSuite() { dump_.reset(); }

    [[nodiscard]] static const std::filesystem::path& path() { return dump_->path; }

protected:
    static std::optional<VcdGen::TempFile> dump_;
    static std::uint64_t steps_;
};

std::optional<VcdGen::TempFile> VcdLarge::dump_;
std::uint64_t VcdLarge::steps_ = 0;

}  // namespace

TEST_F(VcdLarge, ValidateBuilder) {
    Vcd::VcdReader reader{path()};
    CountingBuilder sink;
    auto validateBuilder = makeValidatingBuilder(sink);
    reader.read(*validateBuilder);
}

// Разбор большого дампа выдаёт РОВНО те события, что были записаны: ни одно
// изменение не потеряно и не продублировано на границах буфера чтения.
TEST_F(VcdLarge, ParseEmitsEveryChange) {
    Vcd::VcdReader reader{path()};
    CountingBuilder sink;
    reader.read(sink);

    EXPECT_EQ(sink.times(), steps_);
    EXPECT_EQ(sink.changes(), steps_ * VcdGen::kSignals);
}

// Сквозной путь «дамп -> блочное хранилище на диске -> чтение»: на объёме
// читается ровно то, что записано. Значения сверяются с формулой генератора,
// поэтому эталон хранить не нужно.
TEST_F(VcdLarge, IngestRoundTripMatchesGenerator) {
    const auto store = std::filesystem::path(path()) += ".wsfstore";
    Vcd::VcdReader reader{path()};
    auto builder = makeIndexingBuilder(store);
    const auto db = ingest(reader, *builder);

    ASSERT_TRUE(std::filesystem::exists(store));

    expectMatchesGenerator(db, steps_);
}

// Сквозной путь «дамп -> ленивое хранилище -> VcdWriter -> чтение»: на объёме
// выгрузка воспроизводит те же значения. Коды выходят за один байт, а курсор
// writer'а проходит потоки через границы блоков хранилища.
TEST_F(VcdLarge, RoundTripsLargeDump) {
    const auto store = std::filesystem::path(path()) += ".wsfstore";
    Vcd::VcdReader reader{path()};
    auto builder = makeIndexingBuilder(store);
    const auto db = ingest(reader, *builder);

    const VcdGen::TempFile written{"wasafe_large_written_" + std::to_string(std::random_device{}()) + ".vcd"};
    Vcd::VcdWriter{}.write(db, written.path);

    Vcd::VcdReader rereader{written.path};
    auto rebuilder = makeMemoryBuilder();
    const auto reread = ingest(rereader, *rebuilder);

    expectMatchesGenerator(reread, steps_);
}
