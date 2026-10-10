# Opt-in private xrdp 0.10.6.1 build profile

Source baseline: corrected patch-0041 stack PR #32,
447447ff68fe34cf2391ca9c908c4bd993ed4ef2.
Do **not** build chansrv from the separate Qt/test PRs #34–#39 as a
substitute for the PR #32 source and its zero-fuzz 52-patch replay.

## What the option does

The standard build is unchanged when XRDP_CONSOLE_PRIVATE_XRDP_BUILD=OFF:

- runstatedir=/run
- socketdir=/run/xrdp/sockdir
- localstatedir=<INSTALL_DIR>/var
- the existing user-configured install prefix and build-state fingerprint

A new explicitly opted-in build with
XRDP_CONSOLE_PRIVATE_XRDP_BUILD=ON requires:

- Existing canonical .release/xrdp-console root specified as
  XRDP_CONSOLE_PRIVATE_RELEASE_ROOT.
- An *independent* CMAKE_BINARY_DIR and XRDP_CONSOLE_XRDP_DEPS_ROOT
  strictly under that root. Never reuse a protected build directory.
- No symlinked path components, parent traversals, external/private-prefix
  confusion, or shared runstate/socket directories.
- A hash-keyed private installation prefix under the dependencies root,
  keyed by source/archive, patch series, configure arguments and flags.

The private configure uses runstatedir=<release>/runstate and
socketdir=<release>/socket-root; it retains
localstatedir=<private-hash-install>/var. Thus upstream chansrv's
XRDP_PID_PATH from localstatedir/run and XRDP_SOCKET_ROOT_PATH
from socketdir are both within the private release area.

CMake configuration computes paths but does not create the runstate or
socket directories or any xrdp IPC, and does not start an X11/RDP service.
No default build or production path is rewritten.

## Configuration example — DOCUMENTATION ONLY, NOT EXECUTED

On the trusted Linux host, first manually attest that the chosen workspace
and ancestors are caller-owned, private and not in use. Use a fresh build
directory, separate from the protected build-direct-console tree.

    release=/home/keivan/3DImageMaster/.release/xrdp-console
    cmake -S /home/keivan/3DImageMaster -B "$release/build/abc-private" \
      -DXRDP_CONSOLE_BUILD_XRDP=ON \
      -DXRDP_CONSOLE_PRIVATE_XRDP_BUILD=ON \
      -DXRDP_CONSOLE_PRIVATE_RELEASE_ROOT="$release" \
      -DXRDP_CONSOLE_XRDP_DEPS_ROOT="$release/build/abc-private/_deps"

Source-to-binary proof requires a clean source checkout containing this
CMake change **and** the corrected PR #32 patch stack. The mode refuses
switching between default/private profiles in an existing CMake cache.
A build-only xrdp_upstream target is permissible only under a separately
authorized host build task, never a production install/activation command.
Verify that all resulting installed ELF dependencies, SONAMEs, RUNPATHs,
loadable modules and compiled paths are private before any runtime use.

## What this patch does not solve

A fresh compiled chansrv plus private socketroot is **not sufficient** to
run the C-leg. Codex's host review established that synthetic CLIPRDR
needs a properly private xrdp RDP endpoint forwarding its virtual channel
via chansrvport=DISPLAY(...); correctly scoped module loading, session
handshake and network loopback binding are required. Precreating
<socket-root>/<uid> may bypass a directory-create request, but that is
not a substitute for proving all other sesman/chansrv IPC paths.

This patch does not attest host ownership/ACLs, native library resolution,
Xvfb cookie rejection, Firefox requestor PID, or the trusted screenshot
paste result. No live test is approved.

Offline path/CMake regression:

    python3 -B tests/test_private_xrdp_paths.py

It never builds the dependency, runs xrdp, touches the user's display,
opens a network listener or accesses any clipboard.
