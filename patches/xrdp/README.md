# xrdp patch series

This directory contains the small, production-required delta applied to
pristine xrdp `v0.10.6.1`. The source is downloaded into the ignored
`build/_deps/` tree and is never owned by this repository.

## Upstream pin

* version: `0.10.6.1`
* commit: `80c52879fead585d6347df5b3c587718adf8f5a5`
* archive: <https://github.com/neutrinolabs/xrdp/releases/download/v0.10.6.1/xrdp-0.10.6.1.tar.gz>
* SHA-256: `2f7beb5a3b2529c8d72dc0df9b8cdca31ab0e0c14d1e3421210f5e6ec0ab3b75`

The archive hash and patch order are also recorded in
`docs/xrdp-dependency.md`. Update both manifests together when changing the
upstream pin.

## Series

Apply the files listed in `series` with `patch -p1` from the upstream source
root. The patches are intentionally ordered by behavior rather than by the
historical fork's commit history:

1. `0001-xrdp-resize-state-and-failure-recovery.patch` keeps the VNC resize
   state-machine recovery and propagates resize/update failures safely.
2. `0002-xrdp-fixed-console-vnc-path.patch` keeps fixed-console geometry,
   direct bitmap transport, and end-to-end update error propagation, and
   reserves module code `21` for the first-party physical-console capability.
3. `0003-xrdp-console-input-priority.patch` drains a bounded burst of RDP
   transport work before backend work, followed by one transport check after
   it, only for module code `21`. This keeps queued input and disconnects
   ahead of synchronous direct-X11 graphics work without changing legacy
   VNC/Xorg scheduling.
4. `0004-xrdp-console-own-rfx-encoder.patch` prevents pinned xrdp from
   creating its asynchronous generic encoder for module code `21`, leaving
   synchronous RemoteFX ownership at the first-party module boundary.
5. `0005-librfxcodec-unaligned-stream-access.patch` replaces potentially
   misaligned 16/32-bit typed loads and stores in the x86 RemoteFX stream
   macros with fixed-size `memcpy()` operations. This removes C undefined
   behavior flagged by UBSan without changing the little-endian wire bytes.
6. `0006-xrdp-unaligned-stream-access.patch` replaces potentially misaligned
   little-endian stream loads and stores in `common/parse.h` and the unchecked
   UTF-16 stream helpers in `common/parse.c` with typed local values copied
   using `g_memcpy()`. This removes UBSan-confirmed parser alignment undefined
   behavior while preserving native little-endian bytes.
7. `0007-xrdp-keyboard-layout-hex-conversion.patch` uses `g_atoix()` for the
   two keyboard-layout ID paths. UBSan showed that `g_htoi()` shifts by 32 or
   more bits when parsing normal `0x`-prefixed layout values such as
   `0x00000409`; `g_atoix()` already handles that hexadecimal prefix safely.
   Custom keyboard-layout values written as bare hexadecimal text (for
   example `409`) are a compatibility caveat: `g_htoi()` interpreted that as
   hexadecimal, while `g_atoix()` interprets it as decimal. The distributed
   keyboard configuration uses the supported `0x00000409` form.
8. `0008-xrdp-console-planar-frame-ids.patch` assigns monotonically advancing
   frame IDs to Console Planar fallback frames and stops logging a missing
   generic encoder for each acknowledgement when code 21 intentionally owns
   graphics outside xrdp's asynchronous encoder.
9. `0009-xrdp-console-preserve-gfx-dirty-regions.patch` sends the exact
   pixman dirty rectangles through the Console GFX Planar fallback instead of
   encoding their union bounding box. Legacy sessions retain the upstream
   bounding-box behavior.
10. `0010-xrdp-console-batched-planar-frames.patch` reuses per-session Planar
    compression scratch, sends one bounded dirty-region batch per GFX frame,
    caps region inspection to 32 entries per monitor pass, and commits sent
    stripes or confirmed offscreen stale entries only after a successful frame
    end. Later visible damage remains pending for the next bounded Console
    continuation. Fresh Console damage is coalesced for 16 ms, while a
    successfully sent bounded batch with existing dirty work continues without
    another delay. Failed frames clear the continuation state to avoid a
    zero-delay retry loop. Legacy sessions keep the upstream 40 ms cadence and
    per-rectangle transaction behavior.
11. `0011-xrdp-console-fail-safe-stale-dirty-regions.patch` makes offscreen
    cleanup fail closed when display geometry is missing or invalid, scans a
    fixed 32-rectangle snapshot through the same tested production helper, and
    commits sent/stale rectangles only after a successful frame transaction.
    Its xrdp unit regression verifies a 32-stale prefix cannot hide the visible
    33rd rectangle from the following continuation.
12. `0012-xrdp-console-planar-fresh-continuation-pacing.patch` preserves the
    legacy 40 ms cadence, coalesces fresh Console damage for 16 ms, and removes
    the delay only while draining dirty work left by a successful bounded
    transaction that made measurable dirty-region progress. Successful
    no-progress transactions fall back to the coalescing delay to prevent a
    zero-delay retry loop. The unit tests cover fresh, continuation, completed,
    failed, and no-progress transaction states.

The activation script requires the `XRDP_CONSOLE_GFX_PLANAR_BATCH_V1` marker
in the candidate daemon, so an older patched generation cannot be mistaken
for this Planar batching implementation.

Do not add benchmark instrumentation or first-party runtime code here. These
twelve patches are production compatibility and correctness changes, not
benchmark knobs. Code
`21` is deliberately used only by the direct module's profile; legacy VNC
profiles continue using code `0`/`1`. The
old checked-in fork contained profiling, parser-quantum, request-ahead,
progressive-flush, and experimental GFX changes; those are deliberately
classified as tooling or deleted experiments rather than preserved as
permanent dependency patches.
