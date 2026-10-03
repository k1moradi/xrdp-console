# External review checkpoint: clipboard TARGETS race and screenshot latency

Review this note against repository `main` at code commit
`14fffb9e00a0ca1ddca22b5d27918a1bb91fe310` (`fix stale clipboard TARGETS
retry ordering`). The previous evidence/backlog snapshot is in
[`clipboard-external-review-2026-10-02.md`](clipboard-external-review-2026-10-02.md);
this note updates the status and open questions. All human-facing times below
are America/Los_Angeles (PDT). Raw client logs and local repro files are not
included.

## Checkpoint summary

Commit `14fffb9` adds xrdp patch 0046, a deterministic regression for a stale
local X11 `TARGETS` discovery retry, and clearer reporting when an X11 owner
refuses a selection conversion. This is a narrowly scoped ordering fix. It
does not establish that the reported roughly 71-second screenshot-format
availability delay is solved, nor that Firefox accepted an image in the
12:11 attempt.

The user's hard latency requirement is **under one second** from taking a
macOS screenshot to Linux making its image clipboard target available. Keep
metadata propagation separate from delayed image-payload rendering: the
server should learn/advertise the image formats promptly, while PNG bytes
should remain demand-rendered until an X11 client requests `image/png`.

## What 0046 changes

When chansrv accepts a remote CLIPRDR Format List, it now cancels an active
local X11 conversion only when that conversion is a retrying `TARGETS`
discovery and no CLIPRDR data response is pending. It cancels the retry
policy, clears the local conversion state, and leaves outstanding CLIPRDR
data responses alone. A later X11 `SelectionNotify` for a conversion with no
active retry is logged and ignored. The patch also logs successfully sent
local format lists.

The regression forces the ordering rather than depending on scheduler luck:

1. A competing X11 owner refuses the first `TARGETS` conversion and holds a
   retry request.
2. A newer remote clipboard Format List arrives while an image INCR transfer
   is in progress.
3. The held old `TARGETS` response is released after the newer generation is
   accepted.
4. The test asserts the stale response is ignored, no extra local Format List
   is sent, the in-flight image transfer completes, ownership is restored,
   and text from the latest remote generation remains readable.

The helper now labels a property-`None` refusal as `selection-refused`, not a
timeout. The test also checks that diagnostic distinction.

### Validation status

For `14fffb9`, the author reports: the clipboard session test passed 20
consecutive runs; serial full CTest passed 76/76; clipboard integration
passed with the helper built under ASan/UBSan; and the xrdp patch series
passed strict zero-fuzz replay. An earlier parallel full-suite run had one
transient failure in an unrelated upstream signal test; it passed alone and
the later serial suite passed. This documentation-only checkpoint does not
rerun those code tests.

## Latest screenshot-shortcut incident

### User report

The user reports taking a screenshot at about 12:10 PDT, then finding
Firefox's Edit-menu Paste disabled until about 12:11. After Paste became
available and was clicked, no screenshot upload or visible attachment began.
The user says the macOS Windows App log was cleared before launching a fresh
session, and Linux xrdp-related services were restarted. The desired latency
is less than one second, not roughly one minute.

### Correlated evidence and limits

- Linux logged a text-only client Format List around 12:10:44.7, followed by
  an image Format List containing DIB and named PNG around 12:11:55.3. These
  client-offer timestamps are about 70.6 seconds apart. This does **not** by
  itself measure screenshot-action-to-offer latency: the exact screenshot
  shortcut/pasteboard-update instant is not established by the Windows App
  diagnostic log.
- Linux logged receipt of the image offer's first static-VC fragment around
  12:11:55.330 and chansrv stored the image formats around 12:11:55.457. It
  answered an X11 `TARGETS` request around 12:11:55.573. Thus, in this trace,
  Linux-side handling after the first observed fragment was a few hundred
  milliseconds, not 71 seconds.
- No subsequent X11 `SelectionRequest` for `image/png` or corresponding
  CLIPRDR PNG data request is present in the examined Linux log window after
  the image formats became available. Therefore no PNG payload transfer is
  observed for the failed click. This does not establish why Firefox did not
  issue that request or whether the exact click was correlated to that
  requestor/window.
- The Windows App diagnostic log does not provide a complete, authoritative
  timestamp for the macOS screenshot pasteboard change. Do not describe the
  70.6-second gap between two Format Lists as the proven duration from the
  screenshot keypress.
- No code path was found that rejects clipboard data based on a comparison
  between the Mac and Linux wall clocks. CLIPRDR Format Lists do not carry a
  cross-machine wall-clock timestamp used for this decision; X11 selection
  times are generated/handled on the Linux side. Clock skew is therefore not
  currently evidenced as the cause, but clock synchronization should not be
  assumed when correlating logs.

### Evidence classification

**Observation:** Linux received and exposed DIB/PNG for a generation near
12:11; it answered `TARGETS`; no PNG data request is logged afterward.

**Inference:** the long user-visible wait in this incident occurred before
Linux had an image-format generation to advertise, or in an unobserved client
or event transition. Once Linux received the image offer, the observed
server-side handling was comparatively prompt.

**Not established:** the exact delay from the screenshot shortcut or macOS
pasteboard update to the Windows App offer; whether Windows App observed the
pasteboard immediately; why Firefox did not request `image/png` after the
offer; and whether patch 0046 changes either behavior.

## Open issues

### P0 — sub-second screenshot-format availability and first paste

1. **Meet and measure the user-visible `<1 s` SLO.** Instrument the event
   chain from screenshot shortcut/pasteboard update to Windows App format
   observation, client `CB_FORMAT_LIST` emission, xrdp first-fragment receipt,
   chansrv generation installation, and Linux `TARGETS` response. Use
   per-host monotonic intervals where possible; if wall-clock timestamps are
   correlated, measure/report their offset and uncertainty. Do not reject or
   defer clipboard data based on cross-host wall-clock comparisons.
2. **Determine whether 0046 addresses the observed long wait.** The automated
   test proves the stale local `TARGETS` retry ordering scenario is handled.
   There is not yet a correlated live trace proving that this race caused the
   roughly 71-second user-observed delay, or that the fix brings screenshot
   format availability below one second.
3. **Explain the failed Firefox click after formats were available.** Correlate
   Firefox window/requestor identity, its `TARGETS` request and response, and
   any later `image/png` `SelectionRequest`. In this attempt, no PNG request
   or payload fetch is present in the available Linux logs.
4. **Preserve distinct clipboard failure stages.** Separate (a) no image
   Format List observed at Linux, (b) image offer received late, (c) image
   target advertised but Firefox makes no PNG request, and (d) PNG requested
   but transfer or attachment fails. Do not use one stage's evidence to
   diagnose another.
5. **Complete the exact-payload X11 A/B for the historical generation 6
   incident if it remains necessary.** Syslog records a successful
   owner-side 797,984-byte PNG/INCR transaction and terminator acknowledgement,
   but the exact raw PNG and its hash were not retained. A different
   generation's PNG is not a substitute. Re-capture the exact same payload
   through chansrv and a known-good X11 owner before drawing a semantic
   comparison.

### P1 — protocol and test-oracle confidence

6. **Obtain external review of patch 0046's cancellation boundary.** Confirm
   that cancelling only stale local `TARGETS` discovery on a newer remote
   Format List is correct with active INCR delivery, deferred ownership
   restoration, and pending CLIPRDR data responses. Review whether late X11
   replies and retry timers are harmless across generation changes.
7. **Finish MS-RDPECLIP test-peer capability handling.** The prior review
   identified variable-length capability-set traversal and absent/default
   capability behavior as gaps, along with response correlation and a
   hard-coded PNG numeric format ID. Confirm these are still open against
   current `main` before relying on the helper as a normative oracle.
8. **Add/verify active-session no-feedback coverage.** Prove a remote Format
   List is not echoed as a new local clipboard offer, a genuine Linux owner
   change sends one offer, and restoration of the remote owner sends none.
9. **Keep X11 delay diagnostics responsive.** The delayed-first-INCR test
   helper previously blocked its event loop with `nanosleep()`. Verify whether
   this remains in current `main`; if so, use a monotonic deadline and test
   continued handling of other X11 events while delayed.
10. **Capture authoritative Microsoft macOS clipboard evidence.** The Windows
    App logs so far do not expose a complete pasteboard-change-to-CLIPRDR
    trace. Determine a supported way to observe which formats it sees and
    when it sends the corresponding Format List, without inferring client
    non-emission from Linux-only logs.

### P2 — broader roadmap

11. **Attribute cold PNG transfer time.** WLAN throughput is constrained and
    variable, but its contribution to an individual CLIPRDR transfer remains
    unisolated. Correlate VC fragment timing with contemporaneous radio/TCP
    counters; treat intervals as delay localization, not causal proof.
12. **Review generic static-VC priority/compression.** Keep this at the
    generic RDP transport layer and only implement negotiated, standards-based
    behavior. It is not an established solution for the delayed Format List
    observed in this incident.
13. **Implement standards-compliant file clipboard.** Lazy descriptors,
    `FILECONTENTS_SIZE`/`FILECONTENTS_RANGE`, negotiated locks/large-file
    behavior, and a Linux adapter remain future work.
14. **Complete versioned Microsoft-client profiles.** Record only observed
    values for the exact Windows App/macOS and Windows-client versions; leave
    unknown fields unknown.
15. **Review diagnostic overhead.** The investigation's logging and hashes
    should eventually be disabled or made opt-in on production hot paths,
    particularly on the project's low-power test hardware.

## Advice requested

1. Is patch 0046's cancellation scope correct under MS-RDPECLIP and the
   existing X11 selection/INCR lifecycle? Please review the patch and forced
   ordering test directly, especially the treatment of pending data responses
   and deferred owner restoration.
2. What supported macOS/Windows App instrumentation can establish the
   screenshot pasteboard-change time, formats observed by Windows App, and
   actual client Format List send time? The user requires Linux image-format
   availability in under one second.
3. Given Linux answered `TARGETS` but logged no subsequent `image/png`
   request, what additional X11/Firefox evidence best distinguishes a browser
   that did not request the target from a request that escaped current logs?
4. What should the sub-second SLO's exact endpoints be so it is measurable
   and does not conflict with standards-compliant delayed rendering? The
   intended requirement is prompt format metadata; PNG bytes remain lazy.
5. Are there additional race, generation, or ownership tests required before
   relying on this fix in production?

Please cite MS-RDPECLIP/X11 protocol clauses or specific source locations,
and label observations, inferences, and hypotheses separately. This note is
for review; it does not request a new production behavior change beyond the
already pushed 0046 checkpoint.

## Guardrails

- Do not equate a 70.6-second interval between two logged Format Lists with
  the exact screenshot-action-to-offer latency.
- Do not attribute latency to clock skew without evidence. Do not use
  cross-host wall-clock comparisons as a clipboard validity condition.
- Do not prefetch PNG on Format List, reactivate experimental patch 0042,
  expose partial CLIPRDR/static-VC data to X11, or add a proprietary channel.
- Do not alter H.264 or X11 ownership behavior without a failing, correlated
  regression and a protocol-grounded design review.
- A completed X11 INCR owner-side transaction does not prove Firefox accepted
  the data. A successful screenshot paste does not by itself prove the
  intermittent reliability problem is fixed.
