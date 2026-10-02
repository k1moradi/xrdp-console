# External review: clipboard reliability and conformance

Review base: `9f647802e7adf08264f5733b55d8d4faaa73cd9a` (`main`).

This note requests review and advice; it does not propose a production
clipboard behavior change. All times below are America/Los_Angeles (PDT).
Raw diagnostic archives are intentionally not included in this commit.

## Current evidence

- Screenshot-to-Firefox behavior is intermittent. The user reports that a
  fresh screenshot taken with the macOS screenshot shortcut sometimes does
  not appear in the Linux clipboard, while Preview's Copy Image has worked in
  prior trials. A fresh screenshot paste succeeded again during the latest
  trial.
- In the successful latest trial, chansrv logged generation 17 with DIB format
  8 and named PNG format ID 40005 at about 12:37:34. Firefox subsequently
  requested `image/png`. Chansrv received 585,484 bytes, emitted three X11
  INCR data chunks, logged equal source and XChangeProperty-argument SHA-256
  values (`0aca90f7e056fd6198953902a1abbf6dd98ce84ae067167ad1697e9f9d0dd5e8`),
  and acknowledged the INCR terminator. The user confirmed the paste worked.
  This is a successful live transaction, not a demonstrated fix for
  intermittent failures.
- The attached Microsoft Windows App log contains no explicit textual
  CLIPRDR/clipboard/PNG/DIB event. That means the offer is not identifiable in
  that diagnostic log; it does not prove the client did not emit one. The
  archive covers roughly 12:31:39–12:34:01 PDT, so it does not cover the
  successful generation 17 transaction at 12:37.
- In the Linux chansrv log, the previous image generation before the latest
  success was generation 16 at about 12:36. No corresponding new chansrv image
  generation is present in the earlier 12:31–12:34 window. This establishes
  only that chansrv did not log a new usable image generation in that window.
- Follow-up inspection confirmed that 0043's `XRDP_CONSOLE_RDP_VC` markers are
  written to the systemd journal. For the earlier 12:31:30–12:34:15 window,
  `journalctl -u xrdp` has no CLIPRDR fragment entries, and the chansrv log
  has no new clipboard generation. Thus Linux did not observe a CLIPRDR PDU
  in that window; the evidence still cannot distinguish client non-emission
  from a client-to-server delivery/receive gap.
- The journal later records an image Format List at 12:36:02 and a 1,670,999
  byte PNG response at 12:36:11–12:36:13. Generation 17 and its successful
  Firefox paste follow at 12:37. At 12:46:53 and 12:47:33 the journal records
  two more client-to-server Format List PDUs (`msg_type=2`, `data_len=6`);
  chansrv stores one text format for each. Those later text-only generations
  are not correlated to a specific user action and must not be called
  screenshot supersession without further evidence.
- Raw TCP over the actual Mac/Linux WLAN measured about 6.85 Mbit/s Mac-to-Linux
  and 5.66 Mbit/s Linux-to-Mac in the reported 20-second tests. This is a
  performance constraint, but it has not been shown to cause either missing
  format offers or the full delay of a particular PNG transfer.

## Open issues

### P0 — screenshot offer and first-paste reliability

1. **Intermittent macOS screenshot-shortcut clipboard propagation.** Determine
   for one failing event whether macOS updated its pasteboard, Windows App
   observed the new image formats, it emitted a client `CB_FORMAT_LIST`, xrdp
   received the PDU, and chansrv installed the image generation. Do not infer
   “Windows App did not emit” from an absent Linux event or from this client's
   diagnostic log alone.
2. **Different screenshot creation paths are not yet explained.** The
   screenshot shortcut and Preview's Copy Image have not been compared with
   synchronized client/server evidence. A successful later paste does not
   identify what differed in the earlier attempt.
3. **Intermittent cold PNG paste failures remain unexplained.** Previous
   failures included complete owner-side X11 INCR transactions, while other
   cold transfers succeeded after several seconds. Latency is a possible
   contributor, not a proven universal timeout cause. The latest success is a
   useful positive control, not a root-cause resolution.
4. **No active-session clipboard feedback/restore regression is established.**
   Add a protocol-level test proving that a client-originated Format List is
   not echoed back as a fresh server clipboard update; a genuine Linux owner
   change produces exactly one server offer; and restoring chansrv's remote
   selection produces no synthetic extra offer.
5. **Exact current-HEAD validation is incomplete.** The reported full project
   validation gate was not run after `9f647802`. Re-run the canonical direct
   build/full CTest, strict pristine xrdp patch replay with zero fuzz, and
   focused ASan/UBSan clipboard-helper tests before treating this baseline as
   fully validated.

### P1 — protocol test-peer fidelity and diagnostics

6. **The FreeRDP-backed CLIPRDR peer is not yet a reliable Microsoft-spec
   oracle.** In `tests/helpers/freerdp_cliprdr_overlap_peer.cpp`, capability
   traversal advances with `capabilitySets + index`, rejects an empty
   capability list, and requires a General Capability Set. Variable-length
   sets and Microsoft default behavior need to be handled and unit tested.
7. **The peer hard-codes PNG ID 40005.** Make the remote numeric ID
   configurable and test another ID while preserving name-based PNG mapping.
8. **Format Data Responses are counted but not correlated to a pending
   request.** Track the one outstanding request and reject unsolicited,
   duplicate, or malformed responses in the test peer.
9. **The delayed-first-INCR-chunk helper blocks its X11 event loop with
   `nanosleep()`.** Replace that test-only delay with a monotonic deadline so
   SelectionClear, PropertyNotify, and other requests continue to be served
   during the delay; add responsiveness tests.
10. **The client-side clipboard observation/emission step remains hidden.**
    Linux-side 0043 logging is confirmed in the systemd journal, but the Mac
    log does not expose explicit clipboard events and the failed capture window
    has no received CLIPRDR PDU. Obtain client-side pasteboard/format evidence
    or another reliable client trace to distinguish non-emission from a
    delivery/receive gap. Correlate direction, CLIPRDR message type, generation,
    and timestamps; report all times in Los Angeles local time.
11. **The WLAN contribution to a Class-A transfer is not isolated.** Bracket
    one known cold PNG transaction with immediate station/TCP counter snapshots
    and VC fragment timings, without running iperf concurrently. Treat interval
    measurements as delay localization, not causal attribution.
12. **Protocol correctness and browser performance need separate tests.**
    Keep Firefox-specific timing thresholds out of normative CLIPRDR contract
    tests. Use correlated server log/monotonic timestamps for request, first
    and last VC fragments, PDU completion, SelectionNotify issuance, first X11
    chunk, and terminator acknowledgement.

### P2 — broader clipboard roadmap

13. **Generic static virtual-channel priority/compression work remains open.**
    Review it at the generic RDP transport layer against negotiated Microsoft
    capabilities; it is not an established fix for incoming PNG screenshots.
14. **Standards-compliant file clipboard is not implemented.** The future work
    includes lazy `FileGroupDescriptorW` / `FILECONTENTS_SIZE` /
    `FILECONTENTS_RANGE` behavior, negotiated lock/large-file semantics, and a
    Linux adapter (FUSE remains a candidate, not a protocol dependency).
15. **Captured Microsoft-client profiles are incomplete.** Maintain versioned
    evidence profiles for exact client/platform versions, observed capability
    flags and channel options, initialization sequence, and screenshot format
    offers. Keep unobserved fields explicitly unknown rather than filling
    them from FreeRDP defaults or another Microsoft client version.

## Advice requested from reviewers

1. Verify the capability-set parser/default behavior and initialization state
   against MS-RDPECLIP, including variable-sized and absent capability sets.
2. Review the proposed active-session no-echo/owner-restoration invariant and
   whether the test peer models the normative client sequence without treating
   FreeRDP defaults as Microsoft behavior.
3. Recommend the strongest available way to observe macOS pasteboard changes
   and Windows App format-list emission when its application log has no
   explicit CLIPRDR records.
4. Review patches 0044/0045 (16,256-byte static VC chunk sizing and chansrv IPC
   capacity) for protocol correctness, bounds, and portability.
5. Recommend a minimal measurement plan that separates WLAN throughput,
   client rendering/scheduling, static-VC delivery, xrdp/chansrv processing,
   and X11 owner delivery without changing clipboard semantics.
6. Identify any missing must-have tests or correctness risks before production
   behavior is changed. Please cite protocol clauses or source locations for
   findings, and distinguish observation from inference.

## Guardrails

- Keep screenshot failures where no image generation is observed separate
  from failures after a valid PNG request begins.
- Do not infer client non-emission from Linux-only logs.
- Keep experimental patch 0042 inactive; delayed rendering remains the
  production policy unless a standards-based design review concludes
  otherwise.
- Do not alter X11 ownership semantics, expose partial CLIPRDR data, or modify
  H.264 as part of this review.
- The latest successful paste does not close the end-to-end reliability goal;
  repeatable direct screenshot-to-Firefox/ChatGPT success remains the
  acceptance criterion.
