# Private chansrv ELF closure: static/offline evidence only

This script is a build-validation aid, NOT a private runtime launcher.
The target chansrv source must be the corrected PR #32:
447447ff68fe34cf2391ca9c908c4bd993ed4ef2.

The script runs only readelf -W -d on exact candidate files; it never runs
the target ELF, calls ldd, dlopens a library, touches the clipboard, or
connects to an X11 display.

## Motivation

Codex found that the existing chansrv binary resolves libcommon,
libsesman and libipm from the protected prefix; a different candidate
uses external build-tree RUNPATHs and has no proven PR #32 source identity.
Neither candidate is approved for private runtime.

A fresh, matched build requires independently checked DT_NEEDED,
DT_SONAME, RPATH/RUNPATH and exact file SHA-256 identities. A linker
success or file appearing in .release is insufficient.

## JSON manifest

Provide one existing manifest with these top-level keys:

- schema: integer 1
- release_root: absolute, caller-owned .../.release/xrdp-console
- source_commit: exact full PR #32 SHA listed above
- chansrv: mapping with absolute path and lowercase SHA-256
- private_libraries: mapping from exact native DT_SONAME to another
  mapping with absolute path and lowercase SHA-256

Possible SONAME keys are libcommon.so.0, libsesman.so.0, libipm.so.0,
but these are illustrative: use actual native names observed by Codex.
Every xrdp-private DT_NEEDED must be present in the manifest.

Invoke the read-only audit:

    python3 -B tests/private_elf_closure.py --manifest path/to/manifest.json

The command always returns nonzero exit code 2 and reports
runtime_authorized=false, even when the static private graph passes.

The script rejects protected/external ELF paths, incorrect hashes,
unlisted private SONAMEs, unsafe/ambiguous RUNPATHs and missing private
libcommon/libsesman/libipm. It allows origin-relative ELF paths, and
internal SONAME symlinks only when their canonical target matches the
specific attested artifact under the private release root.

## Remaining NO-GO conditions

This audit does NOT prove executable source provenance merely from the
manifest's source_commit field. A trusted build transcript is still needed.
It does NOT prove the dynamic loader's complete transitive system library
lookup or any hidden dlopen, private sesman/chansrv socket handshake,
runstate isolation, Xvfb identity/authentication, Firefox X11 attribution,
or real macOS screenshot acceptance.

The Linux-host operator must supply exact native source/build and
dependency evidence, all private IPC paths, and process lifecycle contracts.
An independent, specifically authorized private A/B/C runtime test would
still be required later.

## Regression suite

The clipboard-private-elf-closure-unit CTest uses only inert files and
mock readelf output to exercise approved and rejected dependency graphs.
No candidate ELF or browser/clipboard/X11 process is launched.
