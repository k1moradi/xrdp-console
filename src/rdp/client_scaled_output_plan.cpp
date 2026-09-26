// SPDX-License-Identifier: GPL-3.0-or-later

#include "client_scaled_output_plan.h"

#include <climits>

#include "../core/presentation_transform.h"

namespace xrdp_console::rdp
{

bool
clientScaledOutputActivationRequested(const char *value) noexcept
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool
shouldAttemptClientScaledOutputResizeRearm(
    bool pending, bool h264Transport, bool suppressOutput) noexcept
{
    return pending && h264Transport && !suppressOutput;
}

ClientScaledOutputPlanStatus
makeClientScaledOutputDryRunPlan(
    PixelSize source, PixelSize presentation, PixelSize currentFrameGeometry,
    std::size_t maximumFrameBytes,
    ClientScaledOutputDryRunPlan &plan) noexcept
{
    plan = {};
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        presentation.widthPixels == 0 || presentation.heightPixels == 0 ||
        currentFrameGeometry.widthPixels == 0 ||
        currentFrameGeometry.heightPixels == 0)
    {
        return ClientScaledOutputPlanStatus::InvalidGeometry;
    }
    const std::uint64_t currentPixels =
        static_cast<std::uint64_t>(currentFrameGeometry.widthPixels) *
        currentFrameGeometry.heightPixels;
    const std::uint64_t proposedPixels =
        static_cast<std::uint64_t>(source.widthPixels) * source.heightPixels;
    plan.nativeFrameGeometry = source;
    plan.currentCodedPixels = currentPixels;
    plan.proposedCodedPixels = proposedPixels;
    if (proposedPixels < currentPixels)
    {
        plan.reductionBasisPoints = static_cast<std::uint32_t>(
            ((currentPixels - proposedPixels) * UINT64_C(10000)) /
            currentPixels);
    }

    if (source.widthPixels > INT16_MAX || source.heightPixels > INT16_MAX)
    {
        return ClientScaledOutputPlanStatus::NativeGeometryOutOfRange;
    }
    if ((source.widthPixels & 1U) != 0 ||
        (source.heightPixels & 1U) != 0)
    {
        return ClientScaledOutputPlanStatus::NativeGeometryNotAvc420;
    }

    const std::uint64_t proposedFrameBytes =
        proposedPixels + proposedPixels / 2U;
    if (maximumFrameBytes == 0 ||
        proposedFrameBytes > static_cast<std::uint64_t>(maximumFrameBytes))
    {
        return ClientScaledOutputPlanStatus::NativeFrameTooLarge;
    }

    PresentationTransform transform;
    if (!transform.configure(source, presentation))
    {
        return ClientScaledOutputPlanStatus::PresentationTransformUnavailable;
    }

    plan.outputViewport = transform.viewport();
    if (proposedPixels >= currentPixels)
    {
        return ClientScaledOutputPlanStatus::NoCodedPixelReduction;
    }
    return ClientScaledOutputPlanStatus::Eligible;
}

const char *
clientScaledOutputPlanStatusName(ClientScaledOutputPlanStatus status) noexcept
{
    switch (status)
    {
        case ClientScaledOutputPlanStatus::Eligible:
            return "eligible";
        case ClientScaledOutputPlanStatus::InvalidGeometry:
            return "invalid-geometry";
        case ClientScaledOutputPlanStatus::NativeGeometryOutOfRange:
            return "native-geometry-out-of-range";
        case ClientScaledOutputPlanStatus::NativeGeometryNotAvc420:
            return "native-geometry-not-avc420";
        case ClientScaledOutputPlanStatus::NativeFrameTooLarge:
            return "native-frame-too-large";
        case ClientScaledOutputPlanStatus::PresentationTransformUnavailable:
            return "presentation-transform-unavailable";
        case ClientScaledOutputPlanStatus::NoCodedPixelReduction:
            return "no-coded-pixel-reduction";
    }
    return "unknown";
}

} // namespace xrdp_console::rdp
