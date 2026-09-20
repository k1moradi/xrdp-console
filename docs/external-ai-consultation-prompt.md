# External performance review prompt

Copy the prompt below when sending `xrdp-x11vnc-workspace.tar.xz` to an
external AI reviewer. Replace the repository placeholder only after a public
GitHub repository exists.

Create a source-only archive from the project parent. The exclusions protect
against accidentally sending the local Git metadata, the old root-owned
benchmark cache, generated results, or any future deployment artifacts:

```sh
tar -C "$HOME" \
  --exclude='xrdp-x11vnc-workspace/.git' \
  --exclude='xrdp-x11vnc-workspace/benchmarks' \
  --exclude='xrdp-x11vnc-workspace/build*' \
  --exclude='xrdp-x11vnc-workspace/.dist' \
  --exclude='xrdp-x11vnc-workspace/_CPack_Packages' \
  --exclude='xrdp-x11vnc-workspace/results/isolated-runs' \
  --exclude='xrdp-x11vnc-workspace/*.pyc' \
  -cJf "$HOME/xrdp-x11vnc-workspace.tar.xz" \
  xrdp-x11vnc-workspace
```

Before sending it, inspect the archive and confirm that it contains no private
paths or credentials:

```sh
tar -tJf "$HOME/xrdp-x11vnc-workspace.tar.xz"
```

```text
You are reviewing the open-source project xrdp-console. The attached
xrdp-x11vnc-workspace.tar.xz is the authoritative source snapshot. There is no
public GitHub repository yet; use the archive as the source of truth.

This snapshot includes the first release-safe corrections from the previous
review: the private RFB relay now uses bounded selector-driven output buffers;
the input helper reports separate event and X11-draw-complete timestamps; the
benchmark accounts for proxy/Xvfb/chansrv processes; the episode sampler takes
`--rdp-port` and reports CPU PSI; XDamage A/B profiles are orthogonal; and
active Unix sockets/private ports are protected. Please re-audit these changes
and state whether the new tests and measurement model are sufficient before
recommending any xrdp architectural patch.

Please inspect the archive before making recommendations. Do not assume that a
historical result, an external consultation, or a comment is still valid just
because it appears in a document. Distinguish clearly between:

1. results you reproduced from source or local tests;
2. results recorded in docs/validation.md but not reproduced by you;
3. hypotheses and proposed experiments.

Project purpose
---------------
The target workflow is a Windows-like shared console on Lubuntu: a user works
on the physical LXQt/X11 desktop, connects from a Mac RDP client, disconnects,
and later continues at the same physical desktop. The supported architecture
is:

    physical X11 :0 -> x11vnc -> xrdp libvnc.so -> RDP client

The project intentionally does not create a second LXQt session. x11vnc
mirrors the physical display, so the monitor and RDP client operate on the same
processes and windows. The practical requirements are low interactive delay,
held-key repeat, smooth scrolling, reliable clipboard channels, stable
reconnection, no black/frozen screen, and no unbounded CPU/RAM growth.

Machine and observed problem
----------------------------
The original test machine is a low-memory laptop with an Intel Core i3 M330,
about 3.7 GiB RAM, and an AMD RV710/Mobility Radeon GPU using the radeon Mesa
X11 driver. Local OpenGL is hardware accelerated, but Vulkan exposes only
llvmpipe. Local VS Code and Firefox are smooth. Through the xrdp-to-x11vnc
path, VS Code can become very laggy after several minutes while FeatherPad
remains smooth. The slowdown is intermittent and was not explained by a single
codec setting.

The measured console profile uses packaged x11vnc with XDamage, XShm, threads,
keyboard repeat, the LAN speed hint, five-millisecond wait/defer pacing, and
scrollcopyrect disabled. The private benchmark uses classic RFX with
global `[Channels]` `drdynvc=false` plus `[Console]`
`channel.drdynvc=false` for controlled GFX-disabled comparisons;
`disable_gfx=true` is not recognized by the xrdp 0.10.6.1 binary. The live
service and the benchmark are separate; the benchmark never needs to replace
the production binary.

What is in the archive
----------------------
- CMake build with Release/native options, CTest, CPack TGZ/DEB packaging, and
  a GitHub Actions build/test workflow.
- One canonical benchmark: `tools/benchmark/xrdp_console_bench.py`.
- C helpers for X11 pixel/input probes, GL redraw churn, scrolling, and Vulkan
  capability reporting.
- Read-only diagnostic tools for VS Code GPU information and live process,
  memory, PSI, swap, and TCP_INFO sampling.
- Generic systemd templates and documentation; no credentials, Xauthority
  files, private runtime binaries, backups, or raw run logs are part of the
  publication tree. The one canonical optimized xrdp source tree is included
  under `third_party/xrdp-0.10.6.1-optimized` with provenance and notices.

Please review both implementation and project structure. In particular check
that installed paths are relocatable where they should be, the systemd sample
is explicit about privileged deployment, CPack metadata is sensible, the GPLv3
and third-party notices are correct, and the benchmark does not silently touch
production ports or configurations.

Evidence already collected
--------------------------
The clean synthetic network matrix in docs/validation.md used private xrdp,
private x11vnc, FreeRDP on Xvfb, classic RFX with global `[Channels]`
`drdynvc=false` plus `[Console]` `channel.drdynvc=false`, and untouched
production ports. It measured 0, 15, and 30 fps GL redraw churn at 0, 2.5, and
5 ms one-way network delay. The key observations were:

- idle input p50 was about 2-7 ms, while 15/30 fps churn raised input p50 to
  about 14-58 ms;
- graphics p50 was about 62-119 ms and had much larger p95/p99 tails under
  churn;
- round-trip p99 reached about 392 ms in the 15 fps/5 ms case and about
  274 ms in the 30 fps/5 ms case;
- return misses occurred only in two idle control cases, not in the churn
  cases;
- isolated x11vnc used roughly 21-36% CPU and isolated xrdp roughly 53-58%
  during churn.

These results suggest that compositor/capture/encoder scheduling and queueing
are more important than the small synthetic network delay, but they do not
prove which component causes the intermittent VS Code-only slowdown.

Your tasks
----------
1. Report what you tested. Build the project with CMake, run CTest, inspect the
   install tree and CPack metadata, and run any safe static or unit checks that
   your environment supports. If you have a physical X11 display, x11vnc,
   xrdp, FreeRDP, and a readable Xauthority cookie, run a short private
   benchmark. Otherwise say exactly which runtime tests were unavailable.

2. Audit the canonical benchmark for measurement validity. Check timestamp
   placement, pixel-marker correctness, input and graphics decomposition,
   cleanup on failure, namespace/qdisc symmetry, process ownership, and the
   possibility that polling or the benchmark itself adds delay. Identify any
   result that should be rejected or repeated.

3. Analyze the intermittent slowdown as a systems problem. Consider, but do
   not assume, XDamage event behavior, XShm readback, x11vnc capture threads,
   xrdp libvnc scheduling, RFX/GFX negotiation, frame acknowledgements,
   backpressure, TCP buffering, Mac RDP client presentation, VS Code Electron
   rendering/IPC, clipboard/chansrv, swap/PSI pressure, and display scaling.
   Explain which observations support or contradict each hypothesis.

4. Propose major optimization ideas. Rank each idea by expected impact,
   implementation risk, CPU/RAM cost, portability, and how the existing
   benchmark can falsify it. Give a small controlled experiment for every
   high-priority idea. Include the exact command, profile, sample count, and
   acceptance criterion. Prefer reversible private tests over production
   changes.

5. Pay particular attention to experiments that separate these stages:

       RDP input -> xrdp -> x11vnc -> X11 event (T0->T1)
       X11 damage -> capture/encode -> RDP client presentation (T1->T2)
       full user-visible round trip (T0->T2)

   Recommend tests at idle, 15 fps, and 30 fps churn, with 0, 2.5, and 5 ms
   one-way network delay. Preserve p50, p95, p99, maximum, misses, CPU, RSS,
   swap, PSI, and TCP_INFO. Explain how to detect queue growth over time.

6. Review the GPU conclusions. Determine what RV710 hardware can realistically
   accelerate, what Mesa/radeon exposes, and whether any proposed GPU or video
   encoding path is technically possible for this hardware and protocol. Do
   not recommend Vulkan or hardware H.264 encoding merely because local OpenGL
   is accelerated.

7. Review publication quality. Identify portability, security, licensing,
   reproducibility, CMake, packaging, test, documentation, and source-layout
   defects that should be fixed before a public GNU GPLv3 release. Do not ask
   to publish private credentials, system logs containing secrets, or local
   absolute paths.

8. If you recommend code changes, give a concise patch plan tied to the files
   in the archive. Separate changes that are safe for the supported release
   from changes that should remain experimental. Do not present an untested
   patch as a performance result.

Return your answer in this format
---------------------------------
A. Environment and files inspected
B. Tests actually run and exact results
C. Measurement-validity findings
D. Root-cause hypotheses, ranked with evidence
E. Optimization experiments, ranked with commands and pass/fail criteria
F. Publication/CMake/licensing findings
G. Recommended next actions, ordered by value and risk
H. Claims that remain unverified

The useful outcome is evidence-backed guidance and a small number of high-value
experiments. Avoid generic tuning lists and avoid recommending a large rewrite
until a benchmark isolates the responsible stage.
```
