# Third-party components

The runtime bridge uses xrdp, x11vnc, FreeRDP, Xvfb, Mesa, and Vulkan from the
operating system. The optional optimized xrdp daemon is reproduced from the
hash-pinned xrdp 0.10.6.1 archive and the small patch series under
`patches/xrdp/`. CMake downloads and builds it below `build/_deps/`; the
generated source and binaries are not first-party files and are never
installed into the system automatically.

The xrdp build uses the host's development packages by default. Private
dependency locations can be supplied with the CMake cache variables documented
in `docs/xrdp-dependency.md`. No dependency sysroot or generated binary is
checked in.

The following interfaces are used:

- XCB for the first-party X11 module transport;
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
