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
3. `0003-xrdp-console-input-priority.patch` checks the RDP transport before
   and after backend work only for module code `21`. This keeps queued input
   and disconnects ahead of synchronous direct-X11 graphics work without
   changing legacy VNC/Xorg scheduling.

Do not add benchmark instrumentation or first-party runtime code here. These
three patches are production compatibility changes, not benchmark knobs. Code
`21` is deliberately used only by the direct module's profile; legacy VNC
profiles continue using code `0`/`1`. The
old checked-in fork contained profiling, parser-quantum, request-ahead,
progressive-flush, and experimental GFX changes; those are deliberately
classified as tooling or deleted experiments rather than preserved as
permanent dependency patches.
