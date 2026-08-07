#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "usage: $0 <vpp-prefix> <plugin-build-dir> [main-core] [worker-core-list]" >&2
  exit 2
fi

vpp_prefix=$1
plugin_build_dir=$2
main_core=${3:-2}
worker_cores=${4:-3,4}
IFS=, read -r -a worker_core_array <<<"$worker_cores"
worker_count=${#worker_core_array[@]}
if (( worker_count < 1 )); then
  echo "at least one worker core is required" >&2
  exit 2
fi

vpp_bin="$vpp_prefix/bin/vpp"
vppctl_bin="$vpp_prefix/bin/vppctl"
plugin_dir="$plugin_build_dir/lib/vpp_plugins"
tx_client_bin="$plugin_build_dir/bin/uet_vpp_tx_smoke"
owner_probe_bin="$plugin_build_dir/bin/uet_vpp_owner_probe"
endpoint_smoke_bin="$plugin_build_dir/bin/uet_vpp_endpoint_smoke"
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
runtime_dir=$(mktemp -d /tmp/uet-vpp-multiworker.XXXXXX)
config_file="$runtime_dir/startup.conf"
stdout_file="$runtime_dir/stdout.log"
api_prefix="uet-multiworker-$$"
segment_name_a="uet-multiworker-a-$$"
segment_name_b="uet-multiworker-b-$$"
tx_packet_count=${UET_TX_SMOKE_PACKETS:-1024}
vpp_pid=
tx_pid_a=
tx_pid_b=
hold_pid_a=
hold_pid_b=
endpoint_pid_a=
endpoint_pid_b=
collision_holder_pid=

cleanup()
{
  for client_pid in "$collision_holder_pid" "$endpoint_pid_a" "$endpoint_pid_b" \
    "$hold_pid_a" "$hold_pid_b" "$tx_pid_a" "$tx_pid_b"; do
    if [[ -n "$client_pid" ]] && kill -0 "$client_pid" 2>/dev/null; then
      kill "$client_pid"
      wait "$client_pid" 2>/dev/null || true
    fi
  done
  if [[ -n "$vpp_pid" ]] && kill -0 "$vpp_pid" 2>/dev/null; then
    kill "$vpp_pid"
    wait "$vpp_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for required in "$vpp_bin" "$vppctl_bin" "$plugin_dir/uet_plugin.so" \
  "$tx_client_bin" "$owner_probe_bin" "$endpoint_smoke_bin" \
  "$plugin_build_dir/lib/libuet_vpp_client.so"; do
  if [[ ! -e "$required" ]]; then
    echo "missing required file: $required" >&2
    exit 1
  fi
done

sed \
  -e "s|@RUNTIME_DIR@|$runtime_dir|g" \
  -e "s|@API_PREFIX@|$api_prefix|g" \
  -e "s|@PLUGIN_DIR@|$plugin_dir|g" \
  -e "s|@MAIN_CORE@|$main_core|g" \
  -e "s|@WORKER_STANZA@|corelist-workers $worker_cores|g" \
  -e "s|@BUFFERS_PER_NUMA@|${UET_BUFFERS_PER_NUMA:-32768}|g" \
  "$script_dir/startup-worker-contract.conf.in" >"$config_file"

"$vpp_bin" -c "$config_file" >"$stdout_file" 2>&1 &
vpp_pid=$!
for _ in $(seq 1 100); do
  [[ -S "$runtime_dir/cli.sock" ]] && break
  if ! kill -0 "$vpp_pid" 2>/dev/null; then
    cat "$stdout_file" >&2
    exit 1
  fi
  sleep 0.05
done
if [[ ! -S "$runtime_dir/cli.sock" ]]; then
  echo "timed out waiting for VPP CLI socket" >&2
  exit 1
fi

cli=("$vppctl_bin" -s "$runtime_dir/cli.sock")
"${cli[@]}" uet enable
grep -q '^rx-placement current-worker$' <<<"$("${cli[@]}" show uet | tr -d '\r')"
"${cli[@]}" uet rx placement entropy-handoff
grep -q '^rx-placement entropy-handoff$' <<<"$("${cli[@]}" show uet | tr -d '\r')"
"${cli[@]}" uet rx placement current-worker
"${cli[@]}" uet svm create name "$segment_name_a" queue-size 256
"${cli[@]}" uet svm create name "$segment_name_b" queue-size 256

library_path="$plugin_build_dir/lib:$vpp_prefix/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
env LD_LIBRARY_PATH="$library_path" \
  "$tx_client_bin" "$segment_name_a" "$tx_packet_count" "$runtime_dir/uet-dma.sock" \
  >"$runtime_dir/tx-client-a.log" 2>&1 &
tx_pid_a=$!
env LD_LIBRARY_PATH="$library_path" \
  "$tx_client_bin" "$segment_name_b" "$tx_packet_count" "$runtime_dir/uet-dma.sock" \
  >"$runtime_dir/tx-client-b.log" 2>&1 &
tx_pid_b=$!
if ! wait "$tx_pid_a"; then
  tx_pid_a=
  cat "$runtime_dir/tx-client-a.log" >&2
  exit 1
fi
tx_pid_a=
if ! wait "$tx_pid_b"; then
  tx_pid_b=
  cat "$runtime_dir/tx-client-b.log" >&2
  exit 1
fi
tx_pid_b=
cat "$runtime_dir/tx-client-a.log"
cat "$runtime_dir/tx-client-b.log"

status=$("${cli[@]}" show uet | tr -d '\r')
printf '%s\n' "$status"
grep -q "^workers $worker_count$" <<<"$status"
grep -q '^svm-segments 2$' <<<"$status"
grep -q "^application-0-segment $segment_name_a$" <<<"$status"
grep -q "^application-1-segment $segment_name_b$" <<<"$status"
grep -q "^application-0-channels $worker_count$" <<<"$status"
grep -q "^application-1-channels $worker_count$" <<<"$status"
grep -q "^tx-requests $((2 * worker_count * tx_packet_count))$" <<<"$status"
grep -q '^invalid-requests 0$' <<<"$status"
grep -q "^tx-packets $((2 * worker_count * tx_packet_count))$" <<<"$status"
grep -q "^tx-completions $((2 * worker_count * tx_packet_count))$" <<<"$status"
grep -q '^dma-authorized-clients 2$' <<<"$status"
grep -q '^dma-rejected-clients 0$' <<<"$status"
for ((worker = 0; worker < worker_count; worker++)); do
  grep -q "^worker-$worker-channel attached$" <<<"$status"
  grep -q "^worker-$worker-tx-requests $((2 * tx_packet_count))$" <<<"$status"
  grep -q "^worker-$worker-tx-packets $((2 * tx_packet_count))$" <<<"$status"
done

"${cli[@]}" uet svm delete name "$segment_name_a"
status=$("${cli[@]}" show uet | tr -d '\r')
grep -q '^svm-segments 1$' <<<"$status"
grep -q "^svm-segment $segment_name_b$" <<<"$status"
"${cli[@]}" uet svm create name "$segment_name_a" queue-size 256

"${cli[@]}" loopback create
"${cli[@]}" set interface state loop0 up
"${cli[@]}" set interface ip address loop0 198.18.0.1/24
"${cli[@]}" "packet-generator new {
  name uet-ambiguous-rx
  limit 1
  worker 0
  node ip4-input
  interface loop0
  size 64-64
  data {
    UDP: 198.18.0.2 -> 198.18.0.1
    UDP: 10000 -> 49150
    incrementing 36
  }
}"

LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name_a" hold-dma \
  "$runtime_dir/uet-dma.sock" >"$runtime_dir/hold-a.log" 2>&1 &
hold_pid_a=$!
LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name_b" hold-dma \
  "$runtime_dir/uet-dma.sock" >"$runtime_dir/hold-b.log" 2>&1 &
hold_pid_b=$!
for _ in $(seq 1 100); do
  status=$("${cli[@]}" show uet | tr -d '\r')
  if grep -q '^application-0-provider-ready yes$' <<<"$status" &&
    grep -q '^application-1-provider-ready yes$' <<<"$status"; then
    break
  fi
  sleep 0.01
done
grep -q '^application-0-provider-ready yes$' <<<"$status"
grep -q '^application-1-provider-ready yes$' <<<"$status"
"${cli[@]}" packet-generator enable-stream uet-ambiguous-rx
for _ in $(seq 1 100); do
  status=$("${cli[@]}" show uet | tr -d '\r')
  grep -q '^rx-ambiguous 1$' <<<"$status" && break
  sleep 0.01
done
grep -q '^rx-ambiguous 1$' <<<"$status"
grep -q '^rx-delivered 0$' <<<"$status"

kill "$hold_pid_a" "$hold_pid_b"
wait "$hold_pid_a" 2>/dev/null || true
wait "$hold_pid_b" 2>/dev/null || true
hold_pid_a=
hold_pid_b=

"${cli[@]}" uet svm delete name "$segment_name_a"
"${cli[@]}" uet svm delete name "$segment_name_b"
"${cli[@]}" uet svm create name "$segment_name_a" queue-size 256
"${cli[@]}" uet svm create name "$segment_name_b" queue-size 256

LD_LIBRARY_PATH="$library_path" "$endpoint_smoke_bin" "$segment_name_a" \
  "$runtime_dir/uet-dma.sock" hold 777 >"$runtime_dir/collision-holder.log" 2>&1 &
collision_holder_pid=$!
for _ in $(seq 1 500); do
  grep -q '^endpoint control ready: pid 777$' "$runtime_dir/collision-holder.log" 2>/dev/null && break
  sleep 0.01
done
grep -q '^endpoint control ready: pid 777$' "$runtime_dir/collision-holder.log"
LD_LIBRARY_PATH="$library_path" "$endpoint_smoke_bin" "$segment_name_b" \
  "$runtime_dir/uet-dma.sock" expect-collision 777
status=$("${cli[@]}" show uet | tr -d '\r')
grep -q '^endpoint-registrations 1$' <<<"$status"
grep -q '^endpoint-collisions 1$' <<<"$status"
kill -KILL "$collision_holder_pid"
wait "$collision_holder_pid" 2>/dev/null || true
collision_holder_pid=
"${cli[@]}" uet svm delete name "$segment_name_a"
"${cli[@]}" uet svm create name "$segment_name_a" queue-size 256
LD_LIBRARY_PATH="$library_path" "$endpoint_smoke_bin" "$segment_name_b" \
  "$runtime_dir/uet-dma.sock" control 777
"${cli[@]}" show uet | tr -d '\r' | grep -q '^endpoint-registrations 0$'

"${cli[@]}" clear uet counters
LD_LIBRARY_PATH="$library_path" "$endpoint_smoke_bin" "$segment_name_a" \
  "$runtime_dir/uet-dma.sock" >"$runtime_dir/endpoint-a.log" 2>&1 &
endpoint_pid_a=$!
LD_LIBRARY_PATH="$library_path" "$endpoint_smoke_bin" "$segment_name_b" \
  "$runtime_dir/uet-dma.sock" >"$runtime_dir/endpoint-b.log" 2>&1 &
endpoint_pid_b=$!
for _ in $(seq 1 500); do
  if grep -q '^endpoint ready: namespace ' "$runtime_dir/endpoint-a.log" 2>/dev/null &&
    grep -q '^endpoint ready: namespace ' "$runtime_dir/endpoint-b.log" 2>/dev/null; then
    break
  fi
  sleep 0.01
done
grep -q '^endpoint ready: namespace ' "$runtime_dir/endpoint-a.log"
grep -q '^endpoint ready: namespace ' "$runtime_dir/endpoint-b.log"
namespace_a=$(awk '/^endpoint ready: namespace / { print $4; exit }' "$runtime_dir/endpoint-a.log")
namespace_b=$(awk '/^endpoint ready: namespace / { print $4; exit }' "$runtime_dir/endpoint-b.log")
printf -v pid_a_hex '%04x' "$namespace_a"
printf -v pid_b_hex '%04x' "$namespace_b"
printf -v pdc_a_hex '%04x' "$(((namespace_a << 6) | 1))"
printf -v pdc_b_hex '%04x' "$(((namespace_b << 6) | 1))"
printf -v pkt_a_hex '%08x' "$(((namespace_a << 22) | 1))"
printf -v pkt_b_hex '%08x' "$(((namespace_b << 22) | 1))"
printf -v sng_pid_a_hex '%04x' "$namespace_a"
printf -v sng_pid_b_hex '%04x' "$namespace_b"

inject_endpoint_packet()
{
  local name=$1
  local udp_source=$2
  local packet_size=$3
  local payload=$4

  "${cli[@]}" "packet-generator new {
    name $name
    limit 1
    worker 0
    node ip4-input
    interface loop0
    size $packet_size-$packet_size
    data {
      UDP: 198.18.0.2 -> 198.18.0.1
      UDP: $udp_source -> 49150
      hex 0x$payload
    }
  }"
  "${cli[@]}" packet-generator enable-stream "$name"
}

inject_endpoint_routes()
{
  local label=$1
  local pid_hex=$2
  local pdc_hex=$3
  local pkt_hex=$4
  local port_base=$5
  local sng_pid_hex=$6
  local ses="0008000100000000${pid_hex}000f"

  inject_endpoint_packet "uet-$label-rud-syn" "$((port_base + 0))" 52 \
    "11840000000000a100010000${ses}"
  inject_endpoint_packet "uet-$label-rud-established" "$((port_base + 1))" 40 \
    "10000000000000a20001${pdc_hex}"
  inject_endpoint_packet "uet-$label-ack" "$((port_base + 2))" 40 \
    "38000000000000a30001${pdc_hex}"
  inject_endpoint_packet "uet-$label-nack-pdc" "$((port_base + 3))" 44 \
    "50000100000000a40001${pdc_hex}00000000"
  inject_endpoint_packet "uet-$label-control" "$((port_base + 4))" 44 \
    "58800000000000a50001${pdc_hex}00000000"
  inject_endpoint_packet "uet-$label-rudi-request" "$((port_base + 5))" 48 \
    "21800000000000a6${ses}"
  inject_endpoint_packet "uet-$label-rudi-response" "$((port_base + 6))" 36 \
    "28000000${pkt_hex}"
  inject_endpoint_packet "uet-$label-nack-rudi" "$((port_base + 7))" 44 \
    "50080100${pkt_hex}0000000000000000"
  inject_endpoint_packet "uet-$label-uud-request" "$((port_base + 8))" 44 \
    "31800000${ses}"
  inject_endpoint_packet "uet-$label-sng-request" "$((port_base + 9))" 52 \
    "11880000000000a70000000f${ses}"
  inject_endpoint_packet "uet-$label-sng-ack" "$((port_base + 10))" 48 \
    "3a000000000000a8${sng_pid_hex}000f0000000100000000"
}

inject_endpoint_routes endpoint-a "$pid_a_hex" "$pdc_a_hex" "$pkt_a_hex" 10000 \
  "$sng_pid_a_hex"
inject_endpoint_routes endpoint-b "$pid_b_hex" "$pdc_b_hex" "$pkt_b_hex" 10016 \
  "$sng_pid_b_hex"
if ! wait "$endpoint_pid_a"; then
  endpoint_pid_a=
  cat "$runtime_dir/endpoint-a.log" >&2
  exit 1
fi
endpoint_pid_a=
if ! wait "$endpoint_pid_b"; then
  endpoint_pid_b=
  cat "$runtime_dir/endpoint-b.log" >&2
  exit 1
fi
endpoint_pid_b=
grep -q "^endpoint RX namespace $namespace_a passed$" "$runtime_dir/endpoint-a.log"
grep -q "^endpoint RX namespace $namespace_b passed$" "$runtime_dir/endpoint-b.log"
status=$("${cli[@]}" show uet | tr -d '\r')
grep -q '^rx-delivered 22$' <<<"$status"
grep -q '^rx-ambiguous 0$' <<<"$status"
grep -q '^rx-releases 22$' <<<"$status"
grep -q '^endpoint-registrations 0$' <<<"$status"

"${cli[@]}" uet svm delete name "$segment_name_a"
"${cli[@]}" uet svm delete name "$segment_name_b"
echo "UET two-process, $worker_count-worker endpoint demux smoke test passed; logs: $runtime_dir"
