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

1. Resolve and reuse the user's **existing** `.release` workspace.
   Its exact canonical location is deliberately not guessed. The test
   requires `XRDP_CONSOLE_RELEASE_ROOT` to point to a pre-existing,
   owned, private, non-symlinked directory literally named `.release`.
   It stops rather than creating a new dated task/build folder.
2. Keep detached source worktrees and isolated builds under stable paths
   inside that same `.release`. **Do not** run
   `scripts/build-direct-console.sh` blindly: it can rebuild/install
   the pinned xrdp dependency. Never write to or delete the active
   `build-direct-console/_deps/xrdp-install` prefix, even if read-only
   module-loading through its compiled RUNPATH is used.
3. Confirm the intended native `libxrdp_console.so` is built from
   PR #18 commit `d613f9a6dc26482613aa1ee45fd4bb7edee9dbf7`.
   Confirm the exact xrdp and FreeRDP executable provenance,
   unoccupied pinned PID namespace, loaded module path length, and
   bound IPv4 loopback listener. Do not accept a stale production module.
4. The synthetic source and FreeRDP client each require **distinct,
   private** MIT-MAGIC-COOKIE-1 authenticated Xvfb displays.
   The source must reject a wrong cookie, both displays must disable
   TCP, and neither may equal the physical `:0`. The temporary server
   must not launch chansrv or use the user's session bus.
5. After the user **explicitly authorizes** one bounded private RDP test,
   the operator may use the existing isolated CMake test build inside
   `.release`. Set `XRDP_CONSOLE_RELEASE_ROOT` to its verified
   canonical absolute directory and select **only**
   `xrdp-loader-gfx-h264-cropped-edge`, running serially with the
   existing CTest 60-second timeout. Do not run the complete suite,
   reconnect to production or change runtime services.
6. Temporary runtime scratch is kept under `.release/runtime`.
   Last-run synthetic-only logs (at most 128 KiB each) are overwritten
   under `.release/logs/h264-cropped-edge/` with fixed names.
   No new `build/<task-slug>`, home-level task folder, or
   `build-direct-console/test-artifacts/<task-slug>` is permitted.
   Cleanup may only remove verified obsolete **agent-owned** paths;
   preserve pinned dependencies and unrelated user files.

### Example future invocation — NOT authorization to execute

Once the operator has verified an isolated build tree and separately
received the user's approval:

```sh
# Replace these with already-existing, independently verified paths.
export XRDP_CONSOLE_RELEASE_ROOT=/absolute/verified/path/to/.release
# CTEST_BUILD must contain the exact candidate module and the private
# cropped-edge CTest, not the active production build tree.
ctest --test-dir "$CTEST_BUILD" \
  -R '^xrdp-loader-gfx-h264-cropped-edge$' \
  --output-on-failure --no-tests=error -j1
```

This instruction is an acceptance gate, not permission for Codex to
start a private Xvfb, xrdp, FreeRDP or any live clipboard test.

PR #18's original pinned HEAD is `d613f9a6dc26482613aa1ee45fd4bb7edee9dbf7`;
this regression is stacked as a separate draft so Codex can
independently validate that exact fix commit without races.

No test has been run against the physical desktop and this file is not
authorization to restart/reconnect/deploy. The screenshot-paste issue
is independent and tracked under GitHub issue #19.
