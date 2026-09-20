# Third-party components

The current runtime bridge uses xrdp, x11vnc, FreeRDP, Xvfb, Mesa, and Vulkan from
the operating system. The workspace additionally retains the xrdp 0.10.6.1
source used for the optional optimized shared-console daemon under
`third_party/xrdp-0.10.6.1-optimized`; that tree is built locally and is never
installed by the CMake package automatically. Its upstream license and notices
remain in that source tree. This checkout is transitional: the planned
dependency migration will fetch xrdp under `build/_deps/` and apply only the
small required patch series from `patches/xrdp/`.

The benchmark can optionally link the offline codec probe against the system
`rfxcodec` and `x264` development libraries. That target is disabled at
configure time when either dependency is absent. The codec probe is not used
by the end-to-end benchmark.

The optimized xrdp build currently uses the small x86_64 `libxkbfile` development
sysroot under `deps/sysroot-xkbfile`. Its static archive and headers are
redistributed with the permissive notices in
`usr/share/doc/libxkbfile-dev/copyright`; this sysroot is a build convenience,
not a replacement for the host's X11 runtime libraries and is scheduled for
removal in the dependency-migration step.

The following interfaces are used:

- Xlib and XTest for controlled input and pixel probes;
- GLX for the compositor workload and OpenGL probe;
- Vulkan loader for device reporting;
- FreeRDP's `xfreerdp` client for the isolated RDP endpoint;
- xrdp's `libvnc.so` profile for the VNC-to-RDP path;
- Python 3 standard library for orchestration and reporting.

Each dependency is discovered at build or run time. The source distribution
contains systemd deployment templates under `packaging/systemd`; it does not
contain a deployed/private unit, credential, Xauthority cookie, backup, or
machine-specific path.
