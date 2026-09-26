# Synthetic network transport

The unified benchmark can put only the FreeRDP client in a temporary Linux
network namespace. A veth pair connects it to the host-side xrdp process, and
`tc netem` is applied to both directions:

```text
FreeRDP namespace -- veth + netem -- host xrdp-console -- XCB/X11 :0
```

`--network-delay-ms` is a one-way delay. With the same qdisc on both ends, the
expected base RTT is approximately twice the value. Jitter, random loss, and
an optional rate cap use the same per-direction semantics. The first latency
matrix keeps loss at zero so TCP retransmissions do not dominate tail results.

Namespace setup requires a cached sudo ticket:

```sh
sudo -v
python3 -B tools/benchmark/xrdp_console_bench.py \
  --network-self-test --network-delay-ms 2.5 --network-jitter-ms 1
```

Only the short-lived `ip`, `tc`, namespace-entry, and cleanup operations use
`sudo -n`. FreeRDP drops back to the invoking uid/gid before it starts. The
benchmark deletes the namespace in its normal and error cleanup paths.

The matrix runner uses these controls:

| one-way delay | expected RTT | purpose |
| ---: | ---: | --- |
| 0 ms | 0 ms | loopback control |
| 2.5 ms | about 5 ms | good LAN or Wi-Fi |
| 5 ms | about 10 ms | loaded LAN or Wi-Fi |

The supported matrix measures the direct-X11 backend with 0, 15, and 30 fps
GL redraw churn. The runner explicitly selects `--backend direct-x11` and
standard RemoteFX. Zero-delay cases deliberately force jitter to zero and use
loopback; this prevents a misleading comparison in which the control has an
impairment the label does not show.
