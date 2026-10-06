# LXQt menu quality calibration fixtures

These P6 images calibrate the test oracle, not the production encoder.

Run the real-menu stress case from a configured build that has `lxqt-panel`,
`dbus-run-session`, the built FreeRDP client, and an X11 virtual display:

```sh
ctest --test-dir build-direct-console \
    -R '^xrdp-loader-gfx-h264-lxqt-menu-stress$' \
    --output-on-failure
```

Replace `build-direct-console` with the configured build directory. The test
is registered when CMake finds `lxqt-panel` and `dbus-run-session`; reconfigure
after installing either dependency if the test is absent from `ctest -N`.

The test opens the LXQt menu five times while a changing 20 Hz desktop image
and a CPU worker run. Every cycle must deliver a coherent menu within 1000 ms
of the injected client click, pass a sparse fast-response check and a full
menu-region comparison of the same captured images, remain visually stable
for one further capture by default, and show a test-controlled background
epoch after the menu closes. Epoch values change per cycle, so an arbitrary
pixel change or a retained prior popup image cannot mark a cycle fresh.
Artifacts and per-cycle timings are saved under the build's
`test-artifacts/xrdp-loader-gfx-h264-lxqt-menu-stress` directory.

To run a longer stress campaign after the normal five-cycle case, set the
cycle and stability controls explicitly:

```sh
XRDP_CONSOLE_LXQT_MENU_STRESS_CYCLES=20 \
XRDP_CONSOLE_LXQT_STABILITY_SAMPLES=3 \
ctest --test-dir build-direct-console \
    -R '^xrdp-loader-gfx-h264-lxqt-menu-stress$' \
    --output-on-failure
```

The one-second clock starts at the remote click injection and stops when the
client's decoded pixels are sampled for a full image check. It does not include
CTest, server, or FreeRDP startup. A separate quick observer keeps sampling
while the full image checker runs, so image-comparison CPU time cannot consume
the one-second response window. Each full source/client capture has its own
client-pixel timestamp and must be both coherent and within the deadline. The
summary records every quick observation, including failed and over-budget
samples, and every full-check capture timestamp.

- `source-menu.ppm` and `client-01.ppm` through `client-04.ppm` are four
  distinct decoded-client captures from successful cycles 2 through 5 of the
  2026-10-06, five-cycle H.264 LXQt run on this project. The source menu is
  450x550 at source position (0, 498); each client ROI is 354x433 at (0, 442)
  in a 1512x949 presentation of a 1920x1080 source.
- `preopen-roi.ppm` is the corresponding 354x433 crop at (0, 442) from a full
  client frame captured before opening the menu in a separate run on the same
  day. It exercises the negative case where the client still shows the old
  background.
- `live-corrupt-source.ppm` and `live-corrupt-client.ppm` are a source/client
  pair from a later stability sample in the same day's no-contention run. The
  source has clean favorite rows while the decoded client image has overlapping
  rows. The test reports fast outliers of 30.556% and a full-region maximum
  32x32 block mean error of 74.282 for this pair.
- `source-ui-corrupt.ppm` is a source menu captured against the clean `789b648`
  runtime on 2026-10-06 while the LXQt test was also opening its generic
  synthetic popup. The captured source has help text over favorite rows 12 and
  13. Comparing this frame to itself passes the old source-to-client pixel
  oracle, so the test now disables the extra popup and separately checks source
  favorite rows against the clean source fixture. The source-row oracle rejects
  this capture at a maximum per-row mean RGB error of 29.99 (limit 8).

The calibration test requires the identity image and all four captured H.264
pairs to pass both the sparse fast check and full-region check. It requires
pre-open background, the captured live-corrupt pair, an 8-pixel shifted image,
blank pixels, and deliberately corrupted favorite rows to fail both checks.
It reports p50, p95, maximum channel error,
outlier percentage, source luma range, and sample count for each pair.

The fast gate allows at most 20% outliers with p95 <= 108; it screens for
clearly stale or damaged samples before the full-region check. A live H.264
capture on 2026-10-06 scored p95 101 on the sparse points but passed the
full-region comparison at mean RGB error 6.23, p95 53, and maximum block mean
27.6. The retained corrupted capture scores p95 208 and still fails the fast
gate. The full-region check uses tighter calibrated limits: mean RGB error
<= 10, p95 <= 72,
outliers <= 5%, and maximum 32x32 block mean <= 35. The clean captured
full-region pairs measure mean error around 6.3, p95 54-55, outliers around
3.4%, and maximum block mean around 27.8; the retained corrupted pair exceeds
the block limit. These are measurable lossy-H.264 acceptance bounds, not a
claim of zero pixel error. The fixtures come from one FreeRDP/X11 environment
and do not substitute for Microsoft Remote Desktop client validation.

Source validity uses the existing clean menu capture as a row-by-row visual
reference for the 18-pixel favorite row pitch. The source-layout check compares
favorite rows against the fixture and excludes the category pane so system
menu-cache differences do not mask or create favorite-row failures. The
test-only eight-bit epoch is drawn behind the real menu and sampled directly
from the decoded client framebuffer after close. Before opening a cycle, the
test waits for that cycle's epoch to appear at the client; after close, it
waits for the same epoch to reappear. This LXQt source fixture expects the
test's configured panel/menu layout; the H.264 pixel calibration is not a
Windows client or arbitrary desktop-theme compatibility claim.
