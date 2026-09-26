// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace xrdp_console::rdp
{

// Client offloads are the default policy. An unset value enables the path;
// the exact value "1" also enables it. Any explicit non-"1" value disables
// it, so malformed deployment overrides fail closed.
[[nodiscard]] constexpr bool
clientOffloadEnabledByDefault(const char *value) noexcept
{
    return value == nullptr || (value[0] == '1' && value[1] == '\0');
}

} // namespace xrdp_console::rdp
