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

The ordinary test profile opens the LXQt menu five times while a changing 20 Hz
desktop image runs, with one extra full-region capture per popup. It does not
start an extra CPU worker or impose a minimum duration. Every cycle must deliver
a coherent menu within 1000 ms of the injected client click, pass a sparse
response check and a full menu-region comparison of the same captured
source/client images, remain visually stable during the configured extra
captures, and show a test-controlled background epoch after the menu closes.
Epoch values change per cycle, so an arbitrary pixel change or a retained prior
popup image cannot mark a cycle fresh. Artifacts and per-cycle timings are
saved under the build's
`test-artifacts/xrdp-loader-gfx-h264-lxqt-menu-stress` directory.

Run the explicit endurance profile with these controls:

```sh
XRDP_CONSOLE_LXQT_MENU_STRESS_CYCLES=20 \
XRDP_CONSOLE_LXQT_STABILITY_SAMPLES=3 \
XRDP_CONSOLE_LXQT_MIN_STRESS_SECONDS=75 \
XRDP_CONSOLE_LXQT_CPU_CONTENTION=1 \
ctest --test-dir build-direct-console \
    -R '^xrdp-loader-gfx-h264-lxqt-menu-stress$' \
    --output-on-failure
```

The one-second clock starts at the remote click injection and stops when the
client's decoded pixels are sampled. It does not include CTest, server, or
FreeRDP startup. The sparse response check and full-region quality check use
the same captured source/client images; later stability captures are separate
and each must pass the full-region check. The summary separates completed
cycles, accepted popup frames, every fast observation, valid decoded-client
samples, capture errors, full comparisons, and stability captures. The full
comparison's CPU time is outside the one-second freshness measurement.

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

The ten added positive captures came from stress-campaign sequences 000003,
000004, 000005, 000006, 000009, 000010, 000011, 000012, 000014, and 000015.
Their same-frame menu-region comparisons passed. The run later failed its
separate after-close background-epoch check under CPU contention, so these
frames are positive menu-quality calibration only; they do not mean the full
campaign passed. Captures 000004, 000010, and 000015 match the existing
source-menu.ppm byte for byte. The other seven share the additional clean
source-menu-stress.ppm capture (SHA-256
aef74ce91008aa2e11c65d53bcd2cf95f02e93cfe3564629fc2eba4594c4f50b).

| Fixture | Stress capture | Source frame | SHA-256 |
|---|---:|---|---|
| client-05.ppm | 000003 | source-menu-stress.ppm | a9057484d2a241035e5debaafd82208b10cbed2717cb0b9c52c690df88af194f |
| client-06.ppm | 000004 | source-menu.ppm | e012d118c8e2300b3bf6296101cc37476db6f2c6f5c8f4831d939a7bd0f9e72a |
| client-07.ppm | 000005 | source-menu-stress.ppm | c713df917b1a503dea2c646724369f5380af19c8d7e71ee8537d5b6901d4428b |
| client-08.ppm | 000006 | source-menu-stress.ppm | 8e4525f3ada5364a0d815d8be09ca25464a76e8b7a9721ebd3a4a856d3830514 |
| client-09.ppm | 000009 | source-menu-stress.ppm | 75a174f07b2b175cdae924d89bf03c503595622114ad87c0550de10934be184c |
| client-10.ppm | 000010 | source-menu.ppm | 5b4fa74cf91e146edfa22bfbf217d6e8c900d5283caab38c4e9936566171df8b |
| client-11.ppm | 000011 | source-menu-stress.ppm | 194c2a487fb833afb034f9afcd7bc711853fe14a07c2193e2e526942b24d96cc |
| client-12.ppm | 000012 | source-menu-stress.ppm | 99f5af723d01030e54fc6a85c913f43b266cfec7dcc8be2771110f0e992905a4 |
| client-13.ppm | 000014 | source-menu-stress.ppm | 18f70598a11c586c6bc8fdd96855d593205774ac1812a3367adfb6b31e82614d |
| client-14.ppm | 000015 | source-menu.ppm | 29f5eacbc68c1470d6b2e3e1f00e182c664683226fb02ccc028acbe2db81d41e |

The calibration test requires the identity image and all fourteen captured
H.264 pairs to pass both the sparse fast check and full-region check. It
verifies all fourteen decoded-client captures are distinct and checks each
source frame against the clean favorite-row fixture. It requires
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
