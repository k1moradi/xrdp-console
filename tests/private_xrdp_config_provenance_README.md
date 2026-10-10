# Read-only private xrdp configuration provenance

This is a diagnostic CONFIGURATION EVIDENCE tool for corrected xrdp 0.10.6.1.
It is NOT an xrdp launcher, native ELF/runtime closure verifier, replacement
for a build transcript, or permission for any Xvfb/Firefox/RDP test.

Source chain: corrected chansrv PR #32 -> private runstate/socket PR #40
-> sterile compiler/loader environment PR #42 -> this audit PR #43.
Do not substitute the Qt test stack for the corrected chansrv source.

## Inputs

An owned release workspace with a separate, clean Git source worktree at
the reviewed private-build commit and a previously configured CMake build
directory beneath the same workspace. No modified, staged or untracked
source files are accepted; the ordinary production CMakeCache is ineligible.

READ-ONLY command example (NOT EXECUTED here):

    python3 -B tests/private_xrdp_config_provenance.py \
      --source /path/to/.release/xrdp-console/source \
      --build /path/to/.release/xrdp-console/build/abc-private \
      --release /path/to/.release/xrdp-console

## Verified configuration metadata

- Git HEAD read from the actual worktree, with corrected PR #32 as a real
  ancestor, not a supplied source_commit claim; no fetch or checkout
- Exactly 52 ordered patches, series bytes and patch-replay script SHA-256
- Pinned upstream xrdp 0.10.6.1 archive URL and SHA-256
- Configured source/build/release/dependency roots and diagnostic mode
- Private compiler options, no extra CPPFLAGS/LDFLAGS/PKG_CONFIG_PATH
- Independently recomputed patchset and CMake configuration-state hashes
- State-tagged source/build/install paths inside the same private release
- Failures for stale hashes, unsafe flags, external/symlinked paths,
  unexpected archive/profile/patches, forged cache values and missing keys

The output is JSON. It ALWAYS exits status 2 even on complete static
configuration verification, with runtime_authorized=false and
binary_build_or_elf_verified=false.

This check is not an ELF provenance guarantee. Native compiler/linker logs,
upstream patch replay (no fuzz/rejects), actual installed ELF hashes,
DT_NEEDED/SONAME/RUNPATH, private socket/PID paths and RDP/channel handshake
are still independent host gates.

The original checkout's pre-existing ZIP files must not be deleted;
the audited source is a NEW CLEAN WORKTREE inside the release workspace.

## Regression suite

xrdp-private-build-provenance-unit uses synthetic patch files and fake
CMakeCache contents with mocked Git metadata. It performs no native xrdp
compile, install, X11/RDP startup, clipboard or browser operation.
