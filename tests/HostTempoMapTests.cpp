// D-310: HostTempoMap is the only quarter<->second conversion of the project.
// Constant, two tempos, ramp as many points, extrapolation, bar signatures,
// invalid data and the change-detection comparison.
#include "TestAssert.h"

#include "../Source/Audio/Engine/HostTempoMap.h"

#include <cmath>
#include <string>

namespace
{
bool near(double a, double b, double tolerance = 1.0e-9)
{
    return std::abs(a - b) <= tolerance;
}

HostTempoMap twoTempos()
{
    // 240 BPM from q=0 to q=8, then 120 BPM from q=8 to q=12.
    std::vector<HostTempoMap::TempoEntry> entries{{0.0, 0.0}, {2.0, 8.0},
                                                 {4.0, 12.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{0.0, 4, 4}};
    return HostTempoMap::fromEntries(std::move(entries), std::move(sigs));
}

void testConstant()
{
    const HostTempoMap map = HostTempoMap::constant(120.0, 4, 4);
    CHECK(map.isValid());
    CHECK(map.isConstant());
    CHECK(map.getTempoEntries().size() == 2);
    CHECK(map.getBarSignatures().size() == 1);
    CHECK(near(map.timeForQuarter(0.0), 0.0));
    CHECK(near(map.timeForQuarter(4.0), 2.0));
    CHECK(near(map.timeForQuarter(-4.0), -2.0));
    CHECK(near(map.quarterForTime(2.0), 4.0));
    CHECK(near(map.bpmAtTime(3.0), 120.0));
    CHECK(near(map.bpmAtQuarter(17.0), 120.0));
    CHECK(near(map.secondsPerQuarterAtQuarter(0.0), 0.5));
    CHECK(near(map.minSecondsPerQuarter(-8.0, 8.0), 0.5));
    CHECK(map.barStartQuarter(0) == 0.0);
    CHECK(map.barStartQuarter(2) == 8.0);
    CHECK(map.barIndexAtQuarter(8.5) == 2);
    CHECK(map.describe().find("entries=2") != std::string::npos);
}

void testTwoTemposAndExtrapolation()
{
    const HostTempoMap map = twoTempos();
    CHECK(map.isValid());
    CHECK(!map.isConstant());

    // Inside each segment.
    CHECK(near(map.timeForQuarter(4.0), 1.0));
    CHECK(near(map.timeForQuarter(10.0), 3.0));
    CHECK(near(map.quarterForTime(3.0), 10.0));

    // At the sync point the new tempo already applies.
    CHECK(near(map.bpmAtQuarter(7.999), 240.0, 1.0e-3));
    CHECK(near(map.bpmAtQuarter(8.0), 120.0));
    CHECK(near(map.bpmAtQuarter(10.0), 120.0));

    // Extrapolation uses the first/last segment slope.
    CHECK(near(map.timeForQuarter(-4.0), -1.0));
    CHECK(near(map.timeForQuarter(16.0), 6.0));
    CHECK(near(map.quarterForTime(-0.5), -2.0));

    // Round trip across both segments.
    for (double q = -6.0; q <= 18.0; q += 0.25)
        CHECK(near(map.quarterForTime(map.timeForQuarter(q)), q, 1.0e-6));

    // The fastest section is the first one.
    CHECK(near(map.minSecondsPerQuarter(0.0, 12.0), 0.25));
    CHECK(near(map.minSecondsPerQuarter(8.5, 11.0), 0.5));
}

void testUnsortedInputIsSorted()
{
    std::vector<HostTempoMap::TempoEntry> entries{{4.0, 12.0}, {0.0, 0.0},
                                                  {2.0, 8.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{32.0, 3, 4}, {0.0, 4, 4}};
    const HostTempoMap map = HostTempoMap::fromEntries(std::move(entries),
                                                       std::move(sigs));
    CHECK(map.isValid());
    CHECK(near(map.getTempoEntries().front().quarter, 0.0));
    CHECK(map.getBarSignatures().front().numerator == 4);
    CHECK(near(map.timeForQuarter(10.0), 3.0));
}

void testBarSignatures()
{
    // 120 BPM, 4/4 until q=16, then 3/4.
    std::vector<HostTempoMap::TempoEntry> entries{{0.0, 0.0}, {0.5, 1.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{0.0, 4, 4}, {16.0, 3, 4}};
    const HostTempoMap map = HostTempoMap::fromEntries(std::move(entries),
                                                       std::move(sigs));
    CHECK(map.isValid());

    CHECK(map.barIndexAtQuarter(0.0) == 0);
    CHECK(map.barIndexAtQuarter(3.999) == 0);
    CHECK(map.barIndexAtQuarter(4.0) == 1);
    CHECK(map.barIndexAtQuarter(15.999) == 3);
    CHECK(map.barIndexAtQuarter(16.0) == 4);
    CHECK(map.barIndexAtQuarter(18.999) == 4);
    CHECK(map.barIndexAtQuarter(19.0) == 5);
    CHECK(map.barIndexAtQuarter(22.0) == 6);

    CHECK(near(map.barStartQuarter(0), 0.0));
    CHECK(near(map.barStartQuarter(1), 4.0));
    CHECK(near(map.barStartQuarter(3), 12.0));
    CHECK(near(map.barStartQuarter(4), 16.0));
    CHECK(near(map.barStartQuarter(5), 19.0));
    CHECK(near(map.barStartQuarter(6), 22.0));
    CHECK(near(map.barStartQuarter(-1), -4.0));
    CHECK(map.barIndexAtQuarter(-4.0) == -1);

    CHECK(map.signatureAtQuarter(0.0).numerator == 4);
    CHECK(map.signatureAtQuarter(15.0).numerator == 4);
    CHECK(map.signatureAtQuarter(16.0).numerator == 3);

    // 7/8: 3.5 quarters per bar, half a quarter per beat.
    std::vector<HostTempoMap::TempoEntry> rampEntries{{0.0, 0.0}, {0.5, 1.0}};
    std::vector<HostTempoMap::BarSignature> oddSigs{{0.0, 7, 8}};
    const HostTempoMap odd = HostTempoMap::fromEntries(std::move(rampEntries),
                                                       std::move(oddSigs));
    CHECK(near(odd.barQuartersFor(odd.getBarSignatures().front()), 3.5));
    CHECK(near(odd.beatQuartersFor(odd.getBarSignatures().front()), 0.5));
    CHECK(near(odd.barStartQuarter(2), 7.0));
}

void testRampAsManyPoints()
{
    // A linear tempo transition: ARA represents it as many sync points, not as
    // an average BPM.
    std::vector<HostTempoMap::TempoEntry> entries{
        {0.0, 0.0}, {0.5, 2.0}, {1.0, 4.0}, {1.6, 6.0}, {2.3, 8.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{0.0, 4, 4}};
    const HostTempoMap map = HostTempoMap::fromEntries(std::move(entries),
                                                       std::move(sigs));
    CHECK(map.isValid());
    CHECK(!map.isConstant());

    for (const auto& e : map.getTempoEntries())
        CHECK(near(map.timeForQuarter(e.quarter), e.timeSeconds, 1.0e-9));

    // Monotonic and exact at the points.
    double previous = -1.0e9;
    for (double q = 0.0; q <= 8.0; q += 0.1)
    {
        const double t = map.timeForQuarter(q);
        CHECK(t > previous);
        previous = t;
    }
    CHECK(near(map.bpmAtQuarter(1.0), 60.0 / 0.25));
    CHECK(near(map.bpmAtQuarter(3.0), 240.0));
    CHECK(near(map.bpmAtQuarter(5.0), 60.0 / 0.3));
    CHECK(near(map.minSecondsPerQuarter(0.0, 8.0), 0.25));
}

void testInvalidData()
{
    std::vector<HostTempoMap::TempoEntry> one{{0.0, 0.0}};
    CHECK(!HostTempoMap::fromEntries(one, {}).isValid());

    // Time does not advance: no usable tempo.
    std::vector<HostTempoMap::TempoEntry> flat{{0.0, 0.0}, {0.0, 4.0}};
    CHECK(!HostTempoMap::fromEntries(flat, {}).isValid());

    // Quarter does not advance.
    std::vector<HostTempoMap::TempoEntry> stuck{{0.0, 0.0}, {1.0, 0.0}};
    CHECK(!HostTempoMap::fromEntries(stuck, {}).isValid());

    // Not finite.
    std::vector<HostTempoMap::TempoEntry> nan{{0.0, 0.0},
                                              {std::nan(""), 4.0}};
    CHECK(!HostTempoMap::fromEntries(nan, {}).isValid());

    // Invalid maps answer with the documented 120 BPM fallback instead of
    // throwing.
    HostTempoMap invalid = HostTempoMap::fromEntries(one, {});
    CHECK(near(invalid.timeForQuarter(4.0), 2.0));
    CHECK(near(invalid.bpmAtTime(0.0), 120.0));
    CHECK(!invalid.isConstant());
    CHECK(invalid.describe().find("invalid") != std::string::npos);
}

void testEquality()
{
    const HostTempoMap a = HostTempoMap::constant(93.0, 3, 4);
    const HostTempoMap b = HostTempoMap::constant(93.0, 3, 4);
    const HostTempoMap c = HostTempoMap::constant(94.0, 3, 4);
    const HostTempoMap d = HostTempoMap::constant(93.0, 6, 8);
    CHECK(a == b);
    CHECK(!(a == c));
    CHECK(!(a == d));
    CHECK(a.equals(b));

    const HostTempoMap e = twoTempos();
    const HostTempoMap f = twoTempos();
    CHECK(e == f);
    CHECK(e != a);
}

void testDegenerateBpm()
{
    // constant() clamps nonsense instead of producing an invalid map.
    const HostTempoMap zero = HostTempoMap::constant(0.0);
    CHECK(zero.isValid());
    CHECK(near(zero.bpmAtTime(0.0), 120.0));

    const HostTempoMap negative = HostTempoMap::constant(-60.0);
    CHECK(negative.isValid());
    CHECK(near(negative.bpmAtTime(0.0), 120.0));
}
} // namespace

int main()
{
    testConstant();
    testTwoTemposAndExtrapolation();
    testUnsortedInputIsSorted();
    testBarSignatures();
    testRampAsManyPoints();
    testInvalidData();
    testEquality();
    testDegenerateBpm();

    std::printf("HostTempoMapTests: all checks passed\n");
    return 0;
}
