# Shared physical-console profile

The Windows-like workflow is achieved by exporting the already logged-in X11
console instead of starting a second desktop:

```text
RDP client -> xrdp libvnc.so -> loopback x11vnc -> Xorg :0 -> LXQt
```

A local login and an RDP connection therefore see the same windows and
processes. x11vnc mirrors the physical display; it does not implement Windows'
private-console lock, so someone at the monitor can see and operate the same
desktop while RDP is connected.

The measured local-network x11vnc arguments are:

```text
-display :0 -localhost -listen 127.0.0.1 -no6
-xdamage -xd_mem 0 -threads -repeat -input_skip 1
-speeds lan -wait 5 -defer 5 -deferupdate 5
-wait_ui 2 -setdefer -1 -scrollcopyrect never
```

The RDP `[Console]` profile points `libvnc.so` at `127.0.0.1:5900` and keeps
the static `cliprdr` channel enabled when the host's `xrdp-chansrv` is
configured. GFX is carried over `drdynvc`; on xrdp 0.10.6.1 it must be
disabled in both `[Channels]` (`drdynvc=false`) and `[Console]`
(`channel.drdynvc=false`) because capability negotiation happens before the
session profile is fully active. A `disable_gfx=true` line is not recognized by
the stock/custom binary used here and must not be treated as proof that GFX was
disabled. Classic RFX is the explicit fallback tested by this project.

The physical console has a fixed Xorg mode, so dynamic RDP monitor resizing
must be disabled in the `[Console]` section. A Mac client can otherwise request
its window size (for example 1512x949); xrdp cannot apply that request through
the x11vnc backend and the client may show a black framebuffer before closing
the connection. The idempotent deployment helper is
`scripts/fix-console-dynamic-resizing.sh`; run it as root and reconnect after
it restarts xrdp.

If the client still negotiates GFX/H.264 and shows a black screen, run
`scripts/fix-console-gfx-black-screen.sh`. It sets
`drdynvc=false` globally and `channel.drdynvc=false` for `[Console]`, leaves
clipboard redirection on, and restarts xrdp with a rollback backup.

The daemon itself must also contain the VNC/GFX compatibility code. xrdp
0.10.6.1 has a resize-state bug for VNC proxy sessions: after a client sends a
different desktop size, the old graphics state can be torn down without
recreating the surfaces, leaving a black framebuffer with only a cursor. The
upstream fix is tracked in [issue #3833](https://github.com/neutrinolabs/xrdp/issues/3833)
and [PR #3755](https://github.com/neutrinolabs/xrdp/pull/3755). This workspace
applies that state-machine fix on top of the VNC input/encoding optimizations
in `third_party/xrdp-0.10.6.1-optimized`.

Build the candidate with `scripts/build-optimized-xrdp.sh`. It uses one worker,
`-O3`, and `-march=native` because this binary is for the current laptop. Use
`scripts/use-matched-xrdp-console-daemon.sh` to switch the service drop-in
reversibly. The script selects the persistent workspace binary and matching
`libvnc.so`, preserves the existing GFX-disabled console settings, and restores
the old drop-in if the service does not remain active.

This activation changes only the xrdp daemon. The existing xrdp-sesman,
xrdp-chansrv, x11vnc service, clipboard channel, and performance profile stay
in place. The build and unit tests are validated locally; the black-screen fix
still requires a fresh Mac-client reconnection on the target machine.

This repository ships a systemd template for a system-prefix installation as a
deployment aid (`/usr/libexec/xrdp-vnc-bench`). It is not installed or enabled
automatically because display-manager Xauthority paths,
VNC password policy, xrdp package layout, and the local user differ between
systems. The template requires an explicit `XRDP_VNC_AUTH` path in
`/etc/default/xrdp-vnc-bench`; this avoids selecting another user's cookie on
multi-user systems. If the display manager has a stable, administrator-reviewed
cookie discovery policy, set `XRDP_VNC_ALLOW_AUTH_DISCOVERY=1` explicitly.
Validate the generated `[Console]` section before enabling it. Keep the VNC
listener on loopback and do not reuse a login or sudo password as the VNC
password.

The project does not replace the distribution x11vnc binary. The optimized
xrdp source is included under `third_party/xrdp-0.10.6.1-optimized` as the
single reproducible daemon candidate; its build output stays below the local
workspace prefix until the administrator explicitly activates it.
