// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>

#include "rdp/client_scaled_output_plan.h"

using xrdp_console::rdp::ClientScaledOutputDryRunPlan;
using xrdp_console::rdp::clientScaledOutputActivationRequested;
using xrdp_console::rdp::ClientScaledOutputPlanStatus;
using xrdp_console::rdp::makeClientScaledOutputDryRunPlan;
using xrdp_console::rdp::shouldAttemptClientScaledOutputResizeRearm;

namespace
{

constexpr std::size_t kMaximumFrameBytes = 64U * 1024U * 1024U;

} // namespace

int
main()
{
    assert(clientScaledOutputActivationRequested("1"));
    assert(!clientScaledOutputActivationRequested(nullptr));
    assert(!clientScaledOutputActivationRequested(""));
    assert(!clientScaledOutputActivationRequested("0"));
    assert(!clientScaledOutputActivationRequested("true"));

    assert(shouldAttemptClientScaledOutputResizeRearm(true, true, false));
    assert(!shouldAttemptClientScaledOutputResizeRearm(false, true, false));
    assert(!shouldAttemptClientScaledOutputResizeRearm(true, false, false));
    assert(!shouldAttemptClientScaledOutputResizeRearm(true, true, true));

    ClientScaledOutputDryRunPlan plan{};
    auto status = makeClientScaledOutputDryRunPlan(
        {1366, 768}, {1512, 949}, {1512, 948},
        kMaximumFrameBytes, plan);
    assert(status == ClientScaledOutputPlanStatus::Eligible);
    assert((plan.nativeFrameGeometry == PixelSize{1366, 768}));
    assert((plan.outputViewport == Rectangle{0, 49, 1512, 850}));
    assert(plan.currentCodedPixels == 1'433'376);
    assert(plan.proposedCodedPixels == 1'049'088);
    assert(plan.reductionBasisPoints == 2680);

    status = makeClientScaledOutputDryRunPlan(
        {1366, 768}, {800, 600}, {800, 600},
        kMaximumFrameBytes, plan);
    assert(status ==
           ClientScaledOutputPlanStatus::NoCodedPixelReduction);
    assert(plan.currentCodedPixels == 480'000);
    assert(plan.proposedCodedPixels == 1'049'088);
    assert(plan.reductionBasisPoints == 0);

    status = makeClientScaledOutputDryRunPlan(
        {1366, 768}, {1366, 768}, {1366, 768},
        kMaximumFrameBytes, plan);
    assert(status ==
           ClientScaledOutputPlanStatus::NoCodedPixelReduction);

    status = makeClientScaledOutputDryRunPlan(
        {1365, 767}, {1512, 949}, {1512, 948},
        kMaximumFrameBytes, plan);
    assert(status ==
           ClientScaledOutputPlanStatus::NativeGeometryNotAvc420);

    status = makeClientScaledOutputDryRunPlan(
        {32768, 768}, {1512, 949}, {1512, 948},
        kMaximumFrameBytes, plan);
    assert(status ==
           ClientScaledOutputPlanStatus::NativeGeometryOutOfRange);

    status = makeClientScaledOutputDryRunPlan(
        {10000, 10000}, {10000, 10000}, {10000, 10000},
        kMaximumFrameBytes, plan);
    assert(status ==
           ClientScaledOutputPlanStatus::NativeFrameTooLarge);

    status = makeClientScaledOutputDryRunPlan(
        {}, {1512, 949}, {1512, 948}, kMaximumFrameBytes, plan);
    assert(status == ClientScaledOutputPlanStatus::InvalidGeometry);

    return 0;
}
