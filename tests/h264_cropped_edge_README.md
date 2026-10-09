# H.264 cropped-capture regression (October 9, 2026)

This test documents the exact recorded identity NV12 failure in the live worker:

- Source and frame: 1366 x 768.
- Capture: X11 root (1152,704), 128 x 64 BGRA, stride 512.
- First tile: absolute (1152,704), 64 x 64, expected capture-local (0,0).
- Client presentation: 1364 x 768.

The C++ unit addition in `test_gfx_avc420_frame.cpp` proves that the
**old absolute-coordinate invocation fails** on that cropped buffer and
the fixed capture-local invocation produces the same NV12 bytes as a
full-frame reference. The test verifies untouched Y/UV pixels outside
the tile, a second capture-edge tile, both row chunks and invalid
capture geometry. This unit can run without an xrdp service or X11.

The isolated integration CTest is
`xrdp-loader-gfx-h264-cropped-edge`. It asks the existing loader to
create a synthetic window at (1152,704) on its **private** 1366x768
Xvfb source. A synthetic frame changes a pixel near (1182,734); the
client-side probe must observe the new pixel through a 1364x768
presentation. The server log must continue to show H.264 negotiation
without conversion failure, deferred fallback or committed Planar
fallback. Initial GFX Planar surface batches are not prohibited
because they can occur before the first-party H.264 presentation.

## Validation and safety gates

1. Build from PR #18 plus this stacked test branch in an **isolated**
   worktree and write only to a fresh `build/<task-slug>` directory.
2. Do not reuse the running `build-direct-console` source or install
   prefixes. The active xrdp binaries may depend on libraries in that
   prefix via ELF RUNPATH.
3. Before any loader CTest, verify effective DISPLAY, Xauthority,
   loopback RDP port, isolated FreeRDP invocation and the per-user
   chansrv socket namespace. Do not connect to the actual listener
   at TCP/3389 or start an unintended chansrv on DISPLAY=:0.
4. If those constraints cannot be independently proven, **do not
   execute** the integration CTest; stop and report an isolation blocker.
5. In a verified private environment, run only the C++ unit
   `gfx-avc420-frame-unit` and the one targeted
   `xrdp-loader-gfx-h264-cropped-edge` CTest with output-on-failure,
   serial execution, and a bounded timeout. Do not run the 133-test
   suite or alter pass criteria.

PR #18's original pinned HEAD is `d613f9a6dc26482613aa1ee45fd4bb7edee9dbf7`;
this regression is stacked as a separate draft so Codex can
independently validate that exact fix commit without races.

No test has been run against the physical desktop and this file is not
authorization to restart/reconnect/deploy. The screenshot-paste issue
is independent and tracked under GitHub issue #19.
