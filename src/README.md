# First-party runtime

This directory is reserved for the `xrdp-console` runtime: the direct-X11
capture backend, RDP integration boundary, and shared-console policy.

The current checkout still uses the experimental x11vnc bridge for live
measurements. Its benchmark and diagnostic programs live under `tools/`; the
optimized xrdp checkout remains a transitional build dependency under
`third_party/` until the direct-X11 backend replaces it.
