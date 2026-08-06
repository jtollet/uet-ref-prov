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

cleanup()
{
  for client_pid in "$hold_pid_a" "$hold_pid_b" "$tx_pid_a" "$tx_pid_b"; do
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
  "$tx_client_bin" "$owner_probe_bin" "$plugin_build_dir/lib/libuet_vpp_client.so"; do
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
echo "UET two-process, $worker_count-worker independent-channel smoke test passed; logs: $runtime_dir"
