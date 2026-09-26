# Third-party components

The product is a first-party xrdp module using the pinned xrdp 0.10.6.1
runtime. The build downloads that upstream release and applies the small
production patch series in `patches/xrdp/`; generated source and binaries stay
under `build/_deps/` and are not committed.

The direct Console data path uses:

- XCB and its Damage, MIT-SHM, XFixes, and XTest extensions for direct X11
  connection, capture, damage, cursor, and input;
- xrdp and its chansrv/CLIPRDR implementation for RDP transport and clipboard;
- Python 3 for build, deployment, benchmark, and diagnostic orchestration.

FreeRDP and Xvfb are optional local integration-test tools. OpenGL and Vulkan
are optional diagnostics/benchmark dependencies, not runtime accelerators.
Microsoft's Windows and macOS RDP clients connect directly to xrdp.

The upstream xrdp source distribution may build its own legacy `libvnc.so`
module, but xrdp-console does not select that module, start x11vnc, or use RFB
in its production session path. The old VNC/RFB benchmark modes are deprecated
and opt-in only.
