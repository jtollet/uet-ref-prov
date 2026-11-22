# Scaling one UET endpoint across workers

Run eight independent PDS connections between two fixed IPv4 addresses. The
receiver uses one endpoint with 1, 2, 4 or 8 execution contexts. Each context
has its own PDC pool. The test checks that each connection keeps the same
owner and that every 4 KiB SEND arrives with the expected payload.

The two-port test uses raw Ethernet queue pairs from libibverbs. One RX
thread dispatches frames to pinned workers. The sender has eight fixed
workers and one ACK dispatcher. Its test fixture uses distinct `PIDonFEP`
values to keep the eight streams on independent PDCs. Both sides use the
Verbs build of libuet from [RFC #177](https://github.com/ultraethernet/uet-ref-prov/pull/177)
at `64225fc6be52c41e3705f4b29353ff9a290f30c8`.

## Set up the test

Use a Linux host with Python 3.10 or newer, GCC, Make and Git. Install the
OpenSSL development headers. For the physical test, also install the
libibverbs development headers, the RDMA provider for the NIC, `iproute2`
and `ethtool`. The functional test uses `iproute2` and `iptables`.

```sh
git clone --branch rfc/pds-contexts-benchmark --single-branch \
  https://github.com/jtollet/uet-ref-prov.git uet-pds-bench
cd uet-pds-bench
git worktree add --detach ../uet-pds-candidate 64225fc6be52c41e3705f4b29353ff9a290f30c8
```

## Measure over physical Ethernet

Connect two unused Ethernet RDMA ports directly. Start with both ports
administratively down, with no assigned addresses and no bridge or bond
membership. The runner brings them up and restores them on exit. The test
checks the NIC's `rx_packets_phy` and `tx_packets_phy` counters from
`ethtool -S`. The recorded runs used two 100 GbE ConnectX-5 Ex ports.

Replace `RX_PORT` and `TX_PORT` with the Linux interface names. Pick two
disjoint sets of nine logical CPU numbers. Within each set, the dispatcher
runs on the first CPU and its workers on the next eight. Use separate
physical cores local to the corresponding NIC for performance measurement.
The results path must be new.

```sh
sudo python3 -B bench/pds-contexts/tools/run_two_ip_probe.py \
  --candidate ../uet-pds-candidate --results /tmp/uet-pds-rdma-batch \
  --rdma RX_PORT TX_PORT --rx-cpu 4 --tx-cpu 28 \
  --parallel-tx --rdma-batch --workers 1 2 4 8 \
  --iterations 20000 --window 8 --repeat 5
```

Repeat with a fresh results path and omit `--rdma-batch` to poll and repost
one completion per call. Each run checks 160,000 SENDs across eight PDCs.
Compute the receive rate in Mpps as `1000 * rx.dispatch_frames /
rx.dispatch_wall_ns` for each case in `summary.json`, then take the median
for each worker count. This counts inbound UET frames handled by the RX
dispatcher between the first frame and the final receive completion. ACKs
travel in the opposite direction. A 4 KiB message spans four data frames
at MTU 1500.

## Results

These medians come from five runs per row and polling mode. Both modes
verified 3.2 million SENDs each using the candidate above.

| RX workers | One completion per poll | Up to 16 completions per poll |
| ---: | ---: | ---: |
| 1 | 0.312 Mpps | 0.312 Mpps |
| 2 | 0.618 Mpps | 0.619 Mpps |
| 4 | 1.191 Mpps | 1.202 Mpps |
| 8 | 1.274 Mpps | 1.375 Mpps |

The eight-worker batch run reached 4.41× the one-worker rate. The RX
dispatcher was busy throughout and its queues did not fill. The two JSON
files contain per-run ownership counts, timings and NIC counters.
[Single completion](results/rdma-single.json) · [Batch polling](results/rdma-batch.json).

## Verify ownership without RDMA hardware

Use a Linux VM with at least four available CPUs and support for network
namespaces. This run uses a veth pair. Its single-thread sender builds
from the reference revision with the test-only source identity patch in
`patches/`. The eight receiver threads can share two CPUs for this check.

```sh
git worktree add --detach ../uet-pds-reference 5b4e0491329b8e28764bca6847e2f6a0eaa84b9b
sudo python3 -B bench/pds-contexts/tools/run_two_ip_probe.py \
  --candidate ../uet-pds-candidate --reference ../uet-pds-reference \
  --results /tmp/uet-pds-veth --rx-cpu 0 --tx-cpu 3 \
  --functional-worker-cpus 2 --workers 1 2 4 8 --iterations 500 --window 8
```

The physical rates include the software distributor and its worker queues,
along with libuet receive and transmit processing. The fixture copies frames
into those queues. The benchmark exercises ordered eager SEND/RECV over
IPv4. VFIO compatibility is covered by a separate guest regression.
