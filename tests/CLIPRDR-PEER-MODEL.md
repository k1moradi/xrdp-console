# CLIPRDR test-peer model

Clipboard integration tests distinguish protocol requirements from observed
client behavior. The FreeRDP-backed helper is a transport/API shell for a
small, explicitly configured MS-RDPECLIP test peer; it is not itself the
conformance oracle and is not a substitute for Microsoft Windows App or
Windows-client captures.

## Normative peer

The helper's `spec-minimal` profile follows the MS-RDPECLIP initialization
exchange: receive server capabilities and Monitor Ready, then send client
capabilities, a temporary-directory PDU, and the current Format List. It waits
for the server's Format List Response. A client clipboard Format List contains
format identifiers only; image bytes are returned only in response to an
actual Format Data Request. On receiving a server Format List, the helper
acknowledges it and does not request any of its formats unless a separate test
action explicitly simulates a local application paste.

This test profile advertises general capability version 1 with no optional
general flags. `/tmp` is the test process's temporary-directory value; file
clipboard features are not negotiated by this profile. These are explicit
test-peer settings, not values attributed to a Microsoft client.

The normative sequence is specified in [MS-RDPECLIP section 1.3.2.1,
Initialization Sequence](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/a5cae3c9-170c-4154-992d-9ac8a149cc7e).

## Captured Microsoft profiles

Captured behavior is a separate evidence set, keyed by the exact client
product/version and capture. A profile should record only values actually
observed, including static-channel options, virtual-channel capability flags
and chunk size, CLIPRDR capability version/flags, initialization PDU order,
format names and IDs, and measured Format Data Request/Response timings. Keep
the raw capture or log reference and its SHA-256 alongside the profile.

Do not fill unknown fields from FreeRDP defaults, protocol examples, another
Microsoft client/version, or assumptions. At this revision there is no
complete checked-in Microsoft-client CLIPRDR profile; the test helper's
`spec-minimal` profile must not be described as one.
