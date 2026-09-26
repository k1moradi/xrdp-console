// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

#include "../core/geometry.h"
#include "../core/rectangle.h"

namespace xrdp_console::rdp
{

enum class ClientScaledOutputPlanStatus : std::uint8_t
{
    Eligible,
    InvalidGeometry,
    NativeGeometryOutOfRange,
    NativeGeometryNotAvc420,
    NativeFrameTooLarge,
    PresentationTransformUnavailable,
    NoCodedPixelReduction,
};

struct ClientScaledOutputDryRunPlan final
{
    PixelSize nativeFrameGeometry{};
    Rectangle outputViewport{};
    std::uint64_t currentCodedPixels{};
    std::uint64_t proposedCodedPixels{};
    std::uint32_t reductionBasisPoints{};
};

[[nodiscard]] bool clientScaledOutputActivationRequested(
    const char *value) noexcept;

[[nodiscard]] bool shouldAttemptClientScaledOutputResizeRearm(
    bool pending, bool h264Transport,
    bool suppressOutput) noexcept;

[[nodiscard]] ClientScaledOutputPlanStatus makeClientScaledOutputDryRunPlan(
    PixelSize source, PixelSize presentation, PixelSize currentFrameGeometry,
    std::size_t maximumFrameBytes,
    ClientScaledOutputDryRunPlan &plan) noexcept;

[[nodiscard]] const char *clientScaledOutputPlanStatusName(
    ClientScaledOutputPlanStatus status) noexcept;

} // namespace xrdp_console::rdp
