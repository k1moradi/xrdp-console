# Standards-first RDP virtual-channel / clipboard blueprint for Codex

## Non-negotiable goal

Build around Microsoft RDP semantics, not compatibility workarounds:

- MS-RDPECLIP delayed rendering remains the default.
- No eager PNG or file-content transfer on FORMAT_LIST.
- No clipboard-specific gzip/zstd, MIME sniffing, or 512-KiB sampling.
- No proprietary extra clipboard channels.
- Generic static-virtual-channel (SVC) compression and priority belong in
  libxrdp, not chansrv/clipboard.c.
- Preserve 0042 as an experiment, but keep it inactive and do not deploy it.
- H.264 is out of scope.
- No live deployment without explicit user authorization.

## Why the work is phased

Pinned xrdp 0.10.6.1 currently stores CHANNEL_DEF flags, ignores the client's
TS_VIRTUALCHANNEL_CAPABILITYSET, emits no server Virtual Channel Capability
Set, sends static channel data without the standard VC compression layer,
dispatches inbound SVC payloads without SVC decompression, uses a fixed MCS
priority byte, and advertises zero DVC-v2 priority charges. An MPPC encoder
exists for Share Data, but a generic receive-side MPPC decoder was not found.

Never advertise a receive capability before its receive path exists.

## Phase A — observational audit (0043 only)

Apply `0043-xrdp-log-vc-negotiation-and-cliprdr-fragment-timing.patch`.

It is diagnostic-only. Do not advertise capabilities, compress/decompress,
change MCS priority, change VC chunking, touch clipboard ownership, touch
delayed rendering, or touch H.264.

Capture from one Microsoft macOS session and separately one Microsoft Windows
session:

- all CHANNEL_DEF names, IDs and options;
- PRI_HIGH / PRI_MED / PRI_LOW;
- COMPRESS_RDP / COMPRESS;
- Client Info INFO_COMPRESSION and CompressionType;
- client TS_VIRTUALCHANNEL_CAPABILITYSET flags:
  VCCAPS_COMPR_SC, VCCAPS_COMPR_CS_8K, optional VCChunkSize;
- confirmation that the server currently emits no VC capability set;
- DRDYNVC negotiated version and priority charges (add observational logging
  only if existing logs are insufficient).

For one screenshot + Firefox Edit -> Paste, correlate precise timestamps:

- T0 chansrv sends CB_FORMAT_DATA_REQUEST for named PNG 40005;
- T1 xrdp logs `cliprdr-first-fragment`;
- T2 xrdp logs `cliprdr-last-fragment`;
- T3 chansrv logs completed CLIPRDR image response / PNG source;
- T4 chansrv logs X11 SelectionNotify / INCR announcement;
- T5 first X11 data chunk;
- T6 INCR terminator ack.

Report T1-T0, T2-T1, T3-T2, T4-T3, maximum idle gap, and T6-T0.

Seeing an early VC fragment is diagnostic only. Do not expose partial VC
fragments to the local clipboard endpoint without proving that MS-RDPBCGR /
MS-RDPECLIP endpoint semantics permit it.

### Phase-A gate

- 0042 absent from active series.
- Active xrdp sequence: 0001..0040, 0041, 0043.
- `git diff --check`.
- Pristine pinned xrdp 0.10.6.1 + all active patches with GNU patch
  `--fuzz=0`.
- Normal validation build, warnings-as-errors.
- Focused clipboard suite unchanged and green.
- Full CTest green.
- Focused ASan/UBSan if practical.
- Report 0043 SHA-256 and candidate binary hashes.
- STOP before deployment.

## Phase B — server-to-client SVC compression

Start only after Phase A confirms the Microsoft client requested channel
compression and advertises VCCAPS_COMPR_SC.

Add generic libxrdp negotiation state:
- client/server VC capability flags;
- negotiated S->C and C->S VC-compression booleans;
- negotiated/default VCChunkSize;
- highest client bulk-compression type;
- a **separate SVC MPPC encoder context**.

Do not reuse the Share Data MPPC encoder; MPPC history is stateful.

For each eligible SVC chunk:
1. Honor CHANNEL_OPTION_COMPRESS_RDP / CHANNEL_OPTION_COMPRESS exactly as
   specified by MS-RDPBCGR.
2. Require negotiated S->C VC compression.
3. Feed the uncompressed chunk to the SVC MPPC encoder using the highest
   client-advertised type also supported by the server.
4. If standard compression succeeds, send compressed bytes and translate MPPC
   state to CHANNEL_PACKET_COMPRESSED / AT_FRONT / FLUSHED /
   CompressionTypeMask.
5. Otherwise send the original bytes.
6. CHANNEL_PDU_HEADER.length remains the total uncompressed channel-message
   size.
7. Preserve FIRST/LAST/SHOW_PROTOCOL and standard chunking.
8. Compress before encryption.
9. No content heuristics.

Tests:
- highly compressible payload;
- incompressible payload / PNG-like bytes;
- mixed compressed/uncompressed sequence with synchronized history;
- AT_FRONT / FLUSHED;
- multi-chunk messages;
- independent FreeRDP round-trip;
- compression option absent -> never compress;
- VCCAPS_COMPR_SC absent -> never compress;
- graphics/input regression and latency smoke.

Do not advertise C->S compression yet.

## Phase C — client-to-server SVC decompression

Implement and independently test a compliant RDP 4.0 8-KiB MPPC decoder.

Only then advertise VCCAPS_COMPR_CS_8K.

Receive order:
1. security verification/decryption;
2. CHANNEL_PDU_HEADER parse;
3. standard VC decompression with dedicated inbound history;
4. VC chunk reassembly;
5. endpoint dispatch.

Fail closed on malformed compression flags, impossible history transitions,
overflow, or declared-length mismatch. Test with independently generated
FreeRDP compressed VC data.

## Phase D — static-channel MCS priority

Separate patch/task.

Pinned xrdp currently sends a fixed MCS priority byte despite retaining
CHANNEL_DEF priority flags. Verify the exact T.125/MS-RDPBCGR mapping first,
then introduce a priority-aware MCS send API and wire-level tests. Do not turn
individual clipboard file contents into proprietary priority classes. Use
bounded FILECONTENTS_RANGE flow control for bulk clipboard file traffic.

## Phase E — DVC QoS

Separate project. Audit Microsoft-client MS-RDPEDYC version and v2/v3
capabilities. Current pinned xrdp sends DVC v2 with zero priority charges and
opens DVCs at priority 0. Do not move CLIPRDR to a proprietary DVC.

## Phase F — file clipboard / Linux adapter

Separate from screenshot and generic transport work.

Wire behavior:
- standard MS-RDPECLIP;
- descriptors/metadata first;
- FILECONTENTS_SIZE/RANGE only on demand;
- no eager file contents;
- implement CB_CAN_LOCK_CLIPDATA before advertising it;
- implement true 64-bit ranges before advertising huge-file support.

Linux target:
- session-private FUSE mount under `$XDG_RUNTIME_DIR`, not
  `~/thinclient_drives/.clipboard`;
- clean RDPECLIP object model / FUSE adapter boundary;
- bounded reads and backpressure;
- portal integration may be an additional frontend.

## Decision after Phase-A timing

**If T1-T0 is already several seconds:** the Microsoft client itself is slow
to provide the first VC fragment. A delayed-rendered server cannot manufacture
the bytes. Keep prefetch disabled and investigate standards-native local
integration semantics.

**If T1-T0 is short but T2-T1 is long:** bytes arrive progressively at the RDP
transport. Do not stream them into X11 unless the protocol endpoint contract
permits processing before complete CLIPRDR-PDU reassembly.

**If T1/T2 are prompt but T4 is late:** the delay is local chansrv/X11 state;
return to the instrumented selection/INCR path.

## Deliverables before any behavior change

Report:
1. active patch sequence;
2. SHA-256 of new patches;
3. pristine zero-fuzz result;
4. build revision and binary hashes;
5. focused/full/sanitizer test results;
6. Microsoft macOS channel options/caps;
7. Microsoft Windows channel options/caps;
8. screenshot T0..T6 timing;
9. Observation / Inference / Hypothesis summary;
10. no deployment unless explicitly authorized.
