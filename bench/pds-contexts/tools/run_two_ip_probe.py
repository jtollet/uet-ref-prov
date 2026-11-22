#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise eight UET PDCs on two fixed IPv4 addresses."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import platform
import re

from bench_support import Process, build, command, digest, logged, physical_ports, wire_counters


@contextmanager
def virtual_link():
    names = [f"uet-two-{os.getpid()}-{side}" for side in (0, 1)]
    namespaces = []
    try:
        for name in names:
            command(["ip", "netns", "add", name])
            namespaces.append(name)
        command(["ip", "link", "add", "se0", "netns", names[0], "type", "veth", "peer",
                 "name", "se0", "netns", names[1]])
        macs = []
        for side, name in enumerate(names, 1):
            command(["ip", "-n", name, "link", "set", "se0", "up"])
            command(["ip", "-n", name, "addr", "add", f"192.0.2.{side}/30", "dev", "se0"])
            command(["ip", "netns", "exec", name, "iptables", "-A", "INPUT", "-p", "253", "-j", "DROP"])
            macs.append(json.loads(command(["ip", "-n", name, "-j", "link", "show", "se0"]))[0]["address"])
        for side, name in enumerate(names):
            command(["ip", "-n", name, "neigh", "replace", f"192.0.2.{2 - side}", "lladdr",
                     macs[1 - side], "nud", "permanent", "dev", "se0"])
        yield names
    finally:
        for name in reversed(namespaces):
            command(["ip", "netns", "del", name])


def run_case(root, binary, receiver, sender, args, workers, ports, names):
    root.mkdir()
    env = {key: val for key, val in os.environ.items() if not key.startswith(("UET_", "LD_"))}
    env.update(UET_PDS="pds", UET_NIC_SHIM="rawsock", UET_IFNAME="se0",
               UET_PDS_TX_TIMEOUT="50", UET_PDS_MAX_TX_RETRIES="50")
    if args.exhaust_retries:
        env.update(UET_PDS_TX_TIMEOUT="20", UET_PDS_MAX_TX_RETRIES="1")
    processes = []
    try:
        tx_role = "parallel-tx" if args.parallel_tx else "tx"
        sides = (("rx", receiver, args.rx_cpu, workers),
                 (tx_role, sender, args.tx_cpu, 8 if args.parallel_tx else workers))
        for side, (role, library, cpu, count) in enumerate(sides):
            proc_env = dict(env, LD_LIBRARY_PATH=str(library))
            if args.tsan:
                proc_env["TSAN_OPTIONS"] = "halt_on_error=1:exitcode=66"
            if args.drop_established and role == "rx":
                proc_env["SE_DROP_ESTABLISHED"] = "1"
            if args.exhaust_retries and role == "rx":
                proc_env["SE_DROP_ALL_DATA"] = "1"
            if args.late_recv and role == "rx":
                proc_env["SE_LATE_RECV"] = "1"
            prefix = ["ip", "netns", "exec", names[side]] if names else []
            if ports:
                proc_env.update(UET_IFNAME=ports[side]["name"], SE_RDMA_DEVICE=ports[side]["device"],
                                SE_IPV4=f"192.0.2.{side + 1}", SE_PEER_MAC=ports[1 - side]["mac"])
            processes.append(Process([*prefix, binary, role, args.iterations, args.window, count, cpu,
                                      args.worker_cpus],
                                     proc_env, root / f"{role}.log"))
        rx, tx = processes
        if any(p.wait("SE_READY") is None for p in processes):
            raise RuntimeError(f"endpoint failed to start in {root.name}")
        mappings = sum(line.split()[2] == "00000000" for line in
                       Path(f"/proc/{rx.process.pid}/maps").read_text().splitlines()
                       if line.endswith(str(receiver / "libuet_verbs.so")))
        assert mappings == 1
        before = [wire_counters(p) for p in ports] if ports else []
        tx.send("go")
        if args.exhaust_retries:
            assert tx.wait("completion error failed: -5", timeout=10) is not None
            assert tx.process.wait(timeout=2) == 1
            tx_log = (root / f"{tx_role}.log").read_text()
            rx_log = (root / "rx.log").read_text()
            assert "max retries exceeded" in tx_log
            assert not any(term in tx_log + rx_log
                           for term in ("AddressSanitizer:", "ThreadSanitizer:", "Segmentation fault"))
            result = {"workers": workers, "retry_exhaustion": "FI_EIO", "sanitizer_reports": 0}
            (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            return result
        tx_result = json.loads(tx.wait("SE_TX", timeout=38))
        if tx.wait("SE_CLOSED", timeout=20) is None or tx.process.wait(timeout=2):
            raise RuntimeError(f"initiator failed to close in {root.name}")
        rx.send("stop")
        rx_result = json.loads(rx.wait("SE_RX", timeout=10))
        if rx.wait("SE_CLOSED") is None or rx.process.wait(timeout=2):
            raise RuntimeError(f"receiver failed to close in {root.name}")
        assert tx_result["messages"] == 8 * args.iterations
        assert rx_result["pdc_count"] == rx_result["established_count"] == 8
        assert rx_result["dropped_frames"] == int(args.drop_established)
        assert rx_result["dispatch_cpu_ns"] > 0 and rx_result["dispatch_wall_ns"] > 0
        assert len(rx_result["owners"]) == workers
        for owner in rx_result["owners"]:
            assert sum(owner["messages"]) == (8 // workers) * args.iterations
        assert all(sum(o["messages"][i] == args.iterations for o in rx_result["owners"]) == 1
                   for i in range(8))
        result = {"workers": workers, "receiver_library_mappings": mappings,
                  "tx": tx_result, "rx": rx_result}
        tx_log = (root / f"{tx_role}.log").read_text()
        rx_log = (root / "rx.log").read_text()
        result["owned_av_checks"] = "SE_AV_OWNERSHIP_OK" in tx_log
        assert not args.require_owned_av or result["owned_av_checks"]
        if args.tsan:
            result["sanitizer_reports"] = (tx_log + rx_log).count("ThreadSanitizer:")
            assert result["sanitizer_reports"] == 0
        if args.drop_established:
            result["tx_retransmissions"] = len(re.findall(r"PSN \d+ retransmit\b", tx_log))
            assert result["tx_retransmissions"] > 0
        if args.late_recv:
            result["tx_no_match"] = tx_log.count("Msg Rsp: No Match")
            result["rx_unexpected"] = rx_log.count("RX: Unexpected Message")
            assert result["tx_no_match"] == result["rx_unexpected"] == workers
        if ports:
            after = [wire_counters(p) for p in ports]
            result["physical_packets"] = [{k: y[k] - x[k] for k in x} for x, y in zip(before, after)]
            assert result["physical_packets"][0]["rx_packets_phy"] >= 8 * args.iterations
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
    finally:
        for p in reversed(processes):
            p.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True, help="clean checkout of the candidate revision")
    parser.add_argument("--reference", type=Path, help="base revision for the single-worker sender fixture")
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--rdma", nargs=2, metavar=("RX_PORT", "TX_PORT"))
    parser.add_argument("--workers", nargs="+", type=int, default=[1, 2, 4, 8])
    parser.add_argument("--rx-cpu", type=int, required=True, help="dispatcher CPU, followed by worker CPUs")
    parser.add_argument("--tx-cpu", type=int, required=True)
    parser.add_argument("--parallel-tx", action="store_true", help="use eight fixed sender workers and an ACK dispatcher")
    parser.add_argument("--rdma-batch", action="store_true", help="poll and repost up to 16 RDMA RX buffers at once")
    parser.add_argument("--tsan", action="store_true", help="instrument both the library and the test")
    parser.add_argument("--drop-established", action="store_true", help="drop one data frame on an established PDC")
    parser.add_argument("--late-recv", action="store_true", help="post receives after the first packet on each worker")
    parser.add_argument("--exhaust-retries", action="store_true", help="drop data to exercise the PDC error path")
    parser.add_argument("--require-owned-av", action="store_true", help="require the scoped address ownership checks")
    parser.add_argument("--functional-worker-cpus", type=int,
                        help="let test workers share CPUs for functional validation in a small VM")
    parser.add_argument("--iterations", type=int, default=2000)
    parser.add_argument("--window", type=int, default=16)
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()
    if os.geteuid() or platform.system() != "Linux":
        parser.error("Linux and root are required")
    if any(w not in (1, 2, 4, 8) for w in args.workers) or len(set(args.workers)) != len(args.workers):
        parser.error("select distinct worker counts from 1, 2, 4 and 8")
    if args.rdma_batch and not args.rdma:
        parser.error("batch polling requires RDMA")
    if args.require_owned_av and not args.parallel_tx:
        parser.error("the address ownership checks require parallel TX")
    if args.late_recv and (not args.parallel_tx or args.workers != [8] or args.drop_established or args.exhaust_retries):
        parser.error("late receives require one PDC per worker with parallel TX and eight RX workers")
    if args.exhaust_retries and (not args.parallel_tx or args.workers != [8] or args.drop_established):
        parser.error("retry exhaustion requires one PDC per worker with parallel TX and eight RX workers")
    if args.functional_worker_cpus is not None:
        if args.rdma or not 1 <= args.functional_worker_cpus <= max(args.workers):
            parser.error("shared CPUs are available only in the functional veth test")
    if not args.parallel_tx and args.reference is None:
        parser.error("the single-worker sender requires --reference")
    args.worker_cpus = args.functional_worker_cpus or max(args.workers)
    rx_cpus = list(range(args.rx_cpu, args.rx_cpu + args.worker_cpus + 1))
    tx_cpus = list(range(args.tx_cpu, args.tx_cpu + 9)) if args.parallel_tx else [args.tx_cpu]
    if set(tx_cpus) & set(rx_cpus) or not set([*rx_cpus, *tx_cpus]) <= os.sched_getaffinity(0):
        parser.error("pin the dispatcher, workers and initiator to distinct available CPUs")
    if not 1 <= args.window <= 32 or args.iterations <= args.window or args.repeat < 1:
        parser.error("use a 1–32 message window shorter than the workload")
    if args.rdma and len(set(args.rdma)) != 2:
        parser.error("select two distinct cabled RDMA ports")

    root = args.results.resolve()
    root.mkdir(mode=0o700)
    fixture_dir = Path(__file__).resolve().parent
    source_patch = fixture_dir.parent / "patches" / "pds-init-source-pid.patch"
    sanitizer = ["-fsanitize=thread", "-O1", "-fno-omit-frame-pointer"] if args.tsan else []
    receiver = root / "receiver"
    build(receiver, args.candidate.resolve(), [], sanitizer)
    if args.parallel_tx:
        sender = receiver
    else:
        sender = root / "sender"
        build(sender, args.reference.resolve(), [source_patch], sanitizer)
    fixture, rdma, binary = fixture_dir / "shared_endpoint_two_ip.c", fixture_dir / "shared_endpoint_rdma.c", root / "traffic"
    mode = ["-DSE_USE_RDMA", "-Wno-address-of-packed-member", rdma, "-libverbs"] if args.rdma else []
    if args.rdma_batch:
        mode.insert(0, "-DSE_BATCH_RDMA")
    if args.parallel_tx:
        mode.insert(0, "-DSE_PARALLEL_TX")
    logged(["gcc", "-Wall", "-Wextra", "-Werror", "-O2", "-g", "-DENABLE_VERBS=1",
            *sanitizer,
            "-I", receiver, "-I", receiver / "nic_shim", "-I", receiver / "util",
            "-isystem", receiver / "libfabric_headers", "-isystem", receiver / "libfabric_headers/include",
            fixture, *mode, "-L", receiver, "-luet_verbs", "-pthread", "-ldl", "-o", binary], root / "fixture-build.log")
    summary = {"arch": platform.machine(), "mode": "rdma" if args.rdma else "veth functional",
               "tsan": args.tsan,
               "late_recv": args.late_recv,
               "iterations_per_pdc": args.iterations, "window": args.window, "rx_cpus": rx_cpus,
               "tx_cpus": tx_cpus, "tx_workers": 8 if args.parallel_tx else 1,
               "sender_patch_sha256": None if args.parallel_tx else digest(source_patch),
               "fixture_sha256": digest(fixture), "cases": []}
    if args.rdma:
        summary["rdma_sha256"] = digest(rdma)
        summary["rdma_rx_header_sha256"] = digest(fixture_dir / "shared_endpoint_rdma.h")
        summary["rdma_batch"] = args.rdma_batch
    try:
        with (physical_ports(args.rdma) if args.rdma else virtual_link()) as link:
            ports, names = (link, None) if args.rdma else (None, link)
            if ports:
                summary["port_speeds_mbps"] = [p["speed_mbps"] for p in ports]
            for i in range(args.repeat):
                for workers in (args.workers[::-1] if i % 2 else args.workers):
                    case = run_case(root / f"r{i}-w{workers}", binary, receiver, sender, args, workers, ports, names)
                    summary["cases"].append(case)
                    outcome = "retry exhaustion verified" if args.exhaust_retries else "eight PDCs verified"
                    print(f"workers={workers} {outcome}", flush=True)
    finally:
        (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
