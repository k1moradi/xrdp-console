# CODEX.md — xrdp-console test-operator contract

This file is the standing contract between the principal developer and Codex.

Read it before every task in this repository. A task-specific goal may add or
override details, but do not infer broader authority than the goal explicitly
grants.

## Roles

The principal developer owns:

- diagnosis and architecture
- source changes and product fixes
- interpretation of ambiguous evidence
- decisions to merge, deploy, restart, reconnect, or broaden testing

Codex is primarily the test/operator agent:

- fetch exact requested refs
- build only requested targets
- run bounded tests
- operate the Linux host only within explicit authorization
- collect exact evidence
- report first failing boundary without inventing conclusions

Codex may autonomously fix, commit, and push narrowly mechanical
compile/build issues encountered while executing an authorized task. This
permission is intentionally limited to fixes whose purpose and effect are
buildability rather than behavior.

Examples that are normally in scope:

- missing declarations or includes
- mechanical kernel/API/type mismatches
- format-string or compiler warnings
- declaration-order problems
- narrow build-system breakage
- equivalent mechanical compatibility fixes required to compile the already
  intended code

For an in-scope mechanical fix:

1. keep the diff minimal and local to the build failure
2. do not opportunistically refactor nearby code
3. run the smallest focused compile/test that proves the fix
4. commit and push it to the current non-protected working branch
5. report the commit SHA, exact failure, exact fix, and validation result
6. continue the authorized task if the fix is green

The principal will review every such Codex-authored fix post-hoc.

Codex is NOT authorized to perform feature development or substantive
behavioral work. In particular, do not autonomously make:

- architecture changes
- algorithm changes
- synchronization redesign
- garbage-collection/lifetime-policy changes
- streaming-policy changes
- compression/layout work
- performance tuning
- protocol-semantics changes
- changes whose correctness depends on choosing new runtime behavior

If a compile/build fix appears likely to affect runtime semantics, or if more
than one plausible behavioral fix exists, STOP and hand the failure back to
the principal instead of guessing.

Outside this narrow compile/build authority, do not independently redesign or
patch product/runtime code unless the task-specific goal explicitly authorizes
that exact behavior.

## Repository safety

Repository: `k1moradi/xrdp-console`

Protected branch: `main`

Current diagnostic work is on:

`diagnostics/clipboard-image-trigger`

Never:

- merge to `main` without explicit authorization
- force-push or rewrite history
- rebase diagnostic work unless explicitly requested
- discard unrelated worktree changes
- touch the pre-existing untracked
  `xrdp-console-simd-xdamage-handoff.zip`
- broaden a narrowly requested diff

Prefer clean detached worktrees for validation.

Always run `git diff --check` for code changes being validated.

## Runtime safety

Do not, unless the current goal explicitly authorizes it:

- install/replace xrdp, libxrdp, chansrv, or the Console module
- restart xrdp or xrdp-sesman
- reboot
- disconnect/reconnect the user
- install/remove service overrides
- change production configuration
- query live clipboard contents
- perform a new screenshot/copy/paste action

If a goal authorizes a bounded live diagnostic, perform only the named
activation/actions and restore the exact pre-test runtime state afterward.

## Known runtime baselines

Historical/expected production xrdp SHA-256:

`930b1b2268f461842925385b11737e62c87bfa253934471d6b9713de060a51b6`

Current pre-existing Console module SHA-256:

`46904e70fee346c6d4d089d928c2a30785e4dbb8c98f1812ab8cc2204ec64246`

That module embeds revision `1aef75a9ae25` and is known stale. Treat it as a
frozen pre-existing artifact for clipboard diagnostics unless a goal
explicitly says otherwise. Do not repeatedly reopen its provenance gate.

Expected chansrv SHA-256:

`0e720ea14cec728a9882cc44705ec0ed111a0dd6f41582a7c924100253c0a3b8`

Historically validated diagnostic xrdp SHA-256:

`7b6c106b88b43a0da4b6ae3bf36d777d0e71af7b4df130299869e7690ae32a9a`

Historically validated diagnostic libxrdp SHA-256:

`8a1992479303bc2699606c0621d0574d9759edd3e5aca516e6e8893db8c4d937`

Validated unchanged X11 session peer:

`/var/tmp/xrdp-console-clipdiag-fd1f73d/build/bin/clipboard-x11-session-peer`

SHA-256:

`242dfca8549aa419662a26d805bdba0449a6a7da41d304d7fab389ebaa5198af`

Historical clean image-harness validation commit:

`fd1f73d2756d8be800027379a33882882b988151`

Do not claim later observer/tooling commits had a full clean xrdp rebuild unless
one was actually performed.

## Clipboard protocol facts already proven

Do not reopen these questions without contradictory evidence.

1. The Microsoft macOS RDP client can publish both image-bearing and text-only
   CLIPRDR Format Lists after connection.
2. Clipboard generation, not X11 owner XID, is logical clipboard identity.
   The same owner XID may be reused across generations.
3. Text delayed rendering has been proven end-to-end with an exact unique test
   marker:
   X11 UTF8_STRING request -> CB_FORMAT_DATA_REQUEST -> successful
   CB_FORMAT_DATA_RESPONSE -> exact returned text.
4. A real macOS screenshot has been proven through the generic image path:
   fresh image Format List -> PNG format -> X11 image/png request ->
   CB_FORMAT_DATA_REQUEST -> successful CB_FORMAT_DATA_RESPONSE ->
   1,807,429 bytes -> X11 INCR delivery -> requestor received all bytes.
5. Therefore do not patch generic CLIPRDR image transport merely because the
   original Firefox/UI symptom exists. The next meaningful product boundary is
   the real application consumer behavior.
6. The historical image transfer's PNG signature is unknown because the old
   probe did not retain payload bytes or validate the signature. Never infer it.
7. Current diagnostic probe code validates only the first eight PNG signature
   bytes in memory and must not print or persist them.

## CLIPRDR correlation rules

MS-RDPECLIP delayed rendering has no proprietary request ID here.

For FORMAT_DATA_REQUEST/RESPONSE correlation:

- assume only the protocol/code's existing single outstanding request
- preserve ordered type-4 -> type-5 direction
- correlate server-side format ID/attempt/generation separately
- never invent a wire request ID
- never infer message type from packet length

For X11 image correlation, human-readable Atom names are diagnostic metadata,
not stable identity.

A real request/terminator may be logged with:

`target=unknown`

Stable identity is based on the available combination of:

- clipboard generation
- requestor XID
- property Atom
- owner XID
- active Format List's PNG/DIB format ID
- monotonic ordering

A failed SelectionNotify may report property `0x0` (X11 `None`). Treat
`0x0`/zero as absence of a stable property, not as a real property identity.

For successful INCR completion, target text is not required. Match the final
terminator acknowledgement by:

- requestor
- property
- generation

## Clipboard privacy

Only print actual clipboard text when the goal deliberately designates a
harmless unique marker and explicitly requests exact verification.

For arbitrary clipboard/image data retain metadata only:

- format IDs
- generations
- status
- sizes
- timing
- delivery path
- hashes/signature booleans when already computed safely

Do not dump or persist arbitrary clipboard payloads.

## Image format policy

PNG first.

BMP fallback is allowed only when all are true:

- PNG was offered
- PNG genuinely failed/refused/was unusable
- clipboard generation is unchanged
- expected chansrv owner is unchanged
- the existing bounded fallback logic authorizes it

Never use BMP after a generation replacement.

## Browser testing

Use a deterministic X11 probe before browser testing when the generic transport
path is not yet established.

The generic transport path is now established.

The current Firefox diagnostic supports a consumer-only mode which:

- uses the existing DISPLAY clipboard
- does not start Xvfb
- does not install an X11 clipboard owner
- launches a fresh geckodriver-managed Firefox profile
- pastes once into the local probe page
- can fail nonzero with `--expect-png`

Do not attach to or terminate the user's normal Firefox profile/processes.

## Evidence and reporting

Report exact observed facts, not broad conclusions.

Always distinguish:

- source commit under test
- branch HEAD
- binary provenance
- runtime provenance
- test result
- inference

When a test fails, stop at the first demonstrated missing transition unless the
goal explicitly asks for further localization.

Do not label an event as caused by a human clipboard action merely because it
occurred in a broad time window.

Prefer source monotonic timestamps for protocol correlation.

## Standing test workflow

Unless a goal says otherwise:

1. fetch refs
2. verify expected ref/HEAD
3. use a clean detached worktree
4. run `git diff --check`
5. run the smallest focused unit/integration tests
6. build only changed helpers/targets
7. on an offline failure, either:
   - apply only an authorized mechanical compile/build fix under the rules
     above, validate it, commit/push it, and continue; or
   - stop and report if the issue is behavioral, ambiguous, or outside that
     narrow authority
8. perform live actions only if explicitly authorized
9. capture bounded evidence
10. clean up temporary test processes/artifacts
11. verify runtime state unchanged/restored
12. report; do not autonomously implement a substantive product/behavior fix

## Communication

Future task messages should be treated as deltas to this file.

If a task says “follow CODEX.md”, do not ask for the standing rules again.

If this file conflicts with an explicit newer task instruction, the newer
explicit instruction wins for that task.

If a requested action is not covered by either this file or the task-specific
authorization, do not broaden scope. Stop and report what authorization or
evidence is missing.
