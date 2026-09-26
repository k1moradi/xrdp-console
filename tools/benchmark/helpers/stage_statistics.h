// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xrdp_console::benchmark
{

struct DurationSummary final
{
    std::uint64_t minimumNanoseconds{};
    double averageNanoseconds{};
    std::uint64_t p50Nanoseconds{};
    std::uint64_t p95Nanoseconds{};
    std::uint64_t p99Nanoseconds{};
    std::uint64_t maximumNanoseconds{};
};

[[nodiscard]] inline bool
summarizeDurations(std::span<const std::uint64_t> samples,
                   DurationSummary &summary)
{
    if (samples.empty())
    {
        return false;
    }

    std::vector<std::uint64_t> ordered(samples.begin(), samples.end());
    std::sort(ordered.begin(), ordered.end());

    long double sum = 0.0L;
    for (const std::uint64_t sample : ordered)
    {
        sum += static_cast<long double>(sample);
    }

    const auto nearestRank = [&ordered](std::size_t percentile) noexcept {
        const std::size_t rank =
            (ordered.size() * percentile + 99U) / 100U;
        return ordered[rank - 1U];
    };

    summary.minimumNanoseconds = ordered.front();
    summary.averageNanoseconds =
        static_cast<double>(sum / static_cast<long double>(ordered.size()));
    summary.p50Nanoseconds = nearestRank(50U);
    summary.p95Nanoseconds = nearestRank(95U);
    summary.p99Nanoseconds = nearestRank(99U);
    summary.maximumNanoseconds = ordered.back();
    return true;
}

} // namespace xrdp_console::benchmark
