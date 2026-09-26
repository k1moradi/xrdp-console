# First-party runtime

`libxrdp_console.so` is the first-party xrdp module. It is built with
`XRDP_CONSOLE_BUILD_XRDP=ON` against the generated, hash-pinned xrdp 0.10.6.1
dependency. The C ABI adapter and exported module entry points are isolated in
`module/`; the C++ context owns the backend lifecycle and protocol-facing
components.

The module connects to the physical X11 display with XCB and integrates its
file descriptor into xrdp's wait loop. `x11/` owns XCB, XDamage, XFixes, XTest,
and persistent MIT-SHM capture. `core/` owns bounded damage, geometry,
aspect-fit presentation transforms, and cache-bounded scaling. `rdp/` owns
capability inspection, classic bitmap/RemoteFX/GFX output adapters, H.264
presentation state, input-aware graphics scheduling, and bounded scroll-motion
observation. `clipboard/` bridges text formats on the RDP `cliprdr` channel to
the X11 `CLIPBOARD` selection.

The physical X11 source remains authoritative: monitor resize changes the RDP
presentation geometry and transform, not the Xorg mode. Mouse coordinates use
the inverse transform, while letterbox input is excluded. RDP graphics are
selected from negotiated capabilities and successfully initialized paths;
the module does not force H.264 or RemoteFX when the client did not negotiate
them. Eligibility for protocol-defined RDPGFX scaled output is currently
diagnostic only; server-side CPU aspect-fit scaling remains the active
presentation path.

Current scope is deliberately bounded. The first-party clipboard bridge is
text-only. Scroll-motion discovery is observational rather than a production
surface-copy accelerator, and fast page scrolling can still show uneven or
tearing updates. The optional GL 3.3/Vulkan probes and pixel-pipeline
benchmarks do not add GPU processing to the runtime.

See the top-level [`README.md`](../README.md) for dependency installation,
build profiles, benchmark commands, and explicit activation/rollback steps.
