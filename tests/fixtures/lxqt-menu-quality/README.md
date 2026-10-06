# LXQt menu quality calibration fixtures

These P6 images calibrate the test oracle, not the production encoder.

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

The calibration test requires the identity image and all four captured H.264
pairs to pass both the sparse fast check and full-region check. It requires
pre-open background, the captured live-corrupt pair, an 8-pixel shifted image,
blank pixels, and deliberately corrupted favorite rows to fail both checks.
It reports p50, p95, maximum channel error,
outlier percentage, source luma range, and sample count for each pair.

The calibrated limits are deliberately fixed at 20% fast-sample outliers with
p95 <= 96, and full-region mean RGB error <= 20, p95 <= 96, outliers <= 10%,
and maximum 32x32 block mean <= 50. The good captured fast pairs have 15.278%
to 16.667% outliers and p95=93; the shifted and corrupted cases fail both
oracles without changing those limits. The fixtures are evidence from one
FreeRDP/X11 environment and do not substitute for Microsoft Remote Desktop
client validation.
