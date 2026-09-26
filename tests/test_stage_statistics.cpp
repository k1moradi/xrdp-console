// SPDX-License-Identifier: GPL-3.0-or-later

#include "stage_statistics.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

bool
empty_samples_are_rejected()
{
    xrdp_console::benchmark::DurationSummary summary{};
    return check(!xrdp_console::benchmark::summarizeDurations({}, summary),
                 "empty duration set was accepted");
}

bool
single_sample_is_exact()
{
    constexpr std::array samples{std::uint64_t{7}};
    xrdp_console::benchmark::DurationSummary summary{};
    return check(xrdp_console::benchmark::summarizeDurations(samples, summary),
                 "single duration was rejected") &&
           check(summary.minimumNanoseconds == 7U &&
                     summary.averageNanoseconds == 7.0 &&
                     summary.p50Nanoseconds == 7U &&
                     summary.p95Nanoseconds == 7U &&
                     summary.p99Nanoseconds == 7U &&
                     summary.maximumNanoseconds == 7U,
                 "single-sample summary is incorrect");
}

bool
nearest_rank_percentiles_are_correct()
{
    constexpr std::array<std::uint64_t, 10> samples{
        100U, 10U, 90U, 20U, 80U, 30U, 70U, 40U, 60U, 50U};
    xrdp_console::benchmark::DurationSummary summary{};
    return check(xrdp_console::benchmark::summarizeDurations(samples, summary),
                 "valid duration set was rejected") &&
           check(summary.minimumNanoseconds == 10U &&
                     summary.averageNanoseconds == 55.0 &&
                     summary.p50Nanoseconds == 50U &&
                     summary.p95Nanoseconds == 100U &&
                     summary.p99Nanoseconds == 100U &&
                     summary.maximumNanoseconds == 100U,
                 "nearest-rank duration summary is incorrect");
}

bool
extreme_durations_do_not_overflow_summary()
{
    constexpr std::array samples{
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max()};
    xrdp_console::benchmark::DurationSummary summary{};
    return check(xrdp_console::benchmark::summarizeDurations(samples, summary),
                 "large valid durations were rejected") &&
           check(summary.minimumNanoseconds ==
                     std::numeric_limits<std::uint64_t>::max() &&
                     summary.maximumNanoseconds ==
                         std::numeric_limits<std::uint64_t>::max() &&
                     summary.averageNanoseconds ==
                         static_cast<double>(
                             std::numeric_limits<std::uint64_t>::max()),
                 "large duration accumulation overflowed");
}

} // namespace

int
main()
{
    bool success = true;
    success &= empty_samples_are_rejected();
    success &= single_sample_is_exact();
    success &= nearest_rank_percentiles_are_correct();
    success &= extreme_durations_do_not_overflow_summary();
    return success ? 0 : 1;
}
