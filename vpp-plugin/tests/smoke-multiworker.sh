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
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
runtime_dir=$(mktemp -d /tmp/uet-vpp-multiworker.XXXXXX)
config_file="$runtime_dir/startup.conf"
stdout_file="$runtime_dir/stdout.log"
api_prefix="uet-multiworker-$$"
segment_name="uet-multiworker-$$"
tx_packet_count=${UET_TX_SMOKE_PACKETS:-1024}
vpp_pid=
client_pids=()

cleanup()
{
  for pid in "${client_pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  if [[ -n "$vpp_pid" ]] && kill -0 "$vpp_pid" 2>/dev/null; then
    kill "$vpp_pid"
    wait "$vpp_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for required in "$vpp_bin" "$vppctl_bin" "$plugin_dir/uet_plugin.so" \
  "$tx_client_bin" "$plugin_build_dir/lib/libuet_vpp_client.so"; do
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
"${cli[@]}" uet svm create name "$segment_name" queue-size 256

library_path="$plugin_build_dir/lib:$vpp_prefix/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
for ((worker = 0; worker < worker_count; worker++)); do
  if (( worker_count == 1 )); then
    channel_name=$segment_name
  else
    channel_name="$segment_name-w$worker"
  fi
  env LD_LIBRARY_PATH="$library_path" \
    "$tx_client_bin" "$channel_name" "$tx_packet_count" "$runtime_dir/uet-dma.sock" \
    >"$runtime_dir/tx-client-$worker.log" 2>&1 &
  client_pids+=("$!")
done

for ((worker = 0; worker < worker_count; worker++)); do
  if ! wait "${client_pids[$worker]}"; then
    cat "$runtime_dir/tx-client-$worker.log" >&2
    exit 1
  fi
  cat "$runtime_dir/tx-client-$worker.log"
done
client_pids=()

status=$("${cli[@]}" show uet | tr -d '\r')
printf '%s\n' "$status"
grep -q "^workers $worker_count$" <<<"$status"
grep -q "^svm-channels $worker_count$" <<<"$status"
grep -q "^tx-requests $((worker_count * tx_packet_count))$" <<<"$status"
grep -q '^invalid-requests 0$' <<<"$status"
grep -q "^tx-packets $((worker_count * tx_packet_count))$" <<<"$status"
grep -q "^tx-completions $((worker_count * tx_packet_count))$" <<<"$status"
grep -q "^dma-authorized-clients $worker_count$" <<<"$status"
grep -q '^dma-rejected-clients 0$' <<<"$status"
for ((worker = 0; worker < worker_count; worker++)); do
  if (( worker_count == 1 )); then
    channel_name=$segment_name
  else
    channel_name="$segment_name-w$worker"
  fi
  grep -q "^worker-$worker-segment $channel_name$" <<<"$status"
  grep -q "^worker-$worker-tx-requests $tx_packet_count$" <<<"$status"
  grep -q "^worker-$worker-tx-packets $tx_packet_count$" <<<"$status"
done

"${cli[@]}" uet svm delete
echo "UET $worker_count-worker independent-channel smoke test passed; logs: $runtime_dir"
