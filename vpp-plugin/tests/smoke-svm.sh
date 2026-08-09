#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "usage: $0 <vpp-prefix> <plugin-build-dir> [main-core] [worker-core]" >&2
  exit 2
fi

vpp_prefix=$1
plugin_build_dir=$2
main_core=${3:-2}
worker_core=${4:-3}

vpp_bin="$vpp_prefix/bin/vpp"
vppctl_bin="$vpp_prefix/bin/vppctl"
vat2_bin="$vpp_prefix/bin/vat2"
plugin_dir="$plugin_build_dir/lib/vpp_plugins"
vat2_plugin_dir="$plugin_build_dir/lib/vat2_plugins"
tx_client_bin="$plugin_build_dir/bin/uet_vpp_tx_smoke"
negative_client_bin="$plugin_build_dir/bin/uet_vpp_spsc_negative"
owner_probe_bin="$plugin_build_dir/bin/uet_vpp_owner_probe"
dma_auth_probe_bin="$plugin_build_dir/bin/uet_dma_auth_probe"
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
svm_abi_header="$script_dir/../uet/svm_abi.h"
svm_abi_major=$(sed -n \
  's/^#define UET_VPP_SVM_ABI_MAJOR[[:space:]][[:space:]]*//p' "$svm_abi_header")
svm_abi_minor=$(sed -n \
  's/^#define UET_VPP_SVM_ABI_MINOR[[:space:]][[:space:]]*//p' "$svm_abi_header")
runtime_dir=$(mktemp -d /tmp/uet-vpp-svm.XXXXXX)
config_file="$runtime_dir/startup.conf"
stdout_file="$runtime_dir/stdout.log"
api_prefix="uet-svm-$$"
segment_name="uet-svm-$$"
queue_depth=${UET_SVM_QUEUE_DEPTH:-257}
tx_packet_count=4096
rx_packet_count=4
ip4_table_id=7
ip6_table_id=8
vpp_pid=
negative_pid=
owner_probe_pid=

if [[ ! "$queue_depth" =~ ^[0-9]+$ ]] || (( queue_depth < 8 || queue_depth > 4096 )); then
  echo "UET_SVM_QUEUE_DEPTH must be between 8 and 4096" >&2
  exit 2
fi

cleanup()
{
  if [[ -n "$owner_probe_pid" ]] && kill -0 "$owner_probe_pid" 2>/dev/null; then
    kill "$owner_probe_pid"
    wait "$owner_probe_pid" 2>/dev/null || true
  fi
  if [[ -n "$negative_pid" ]] && kill -0 "$negative_pid" 2>/dev/null; then
    kill "$negative_pid"
    wait "$negative_pid" 2>/dev/null || true
  fi
  if [[ -n "$vpp_pid" ]] && kill -0 "$vpp_pid" 2>/dev/null; then
    kill "$vpp_pid"
    wait "$vpp_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for required in \
  "$vpp_bin" \
  "$vppctl_bin" \
  "$vat2_bin" \
  "$plugin_dir/uet_plugin.so" \
  "$vat2_plugin_dir/uet_test_plugin_uet_plugin.so" \
  "$tx_client_bin" \
  "$negative_client_bin" \
  "$owner_probe_bin" \
  "$dma_auth_probe_bin" \
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
  -e "s|@WORKER_CORE@|$worker_core|g" \
  "$script_dir/startup-one-worker.conf.in" >"$config_file"

"$vpp_bin" -c "$config_file" >"$stdout_file" 2>&1 &
vpp_pid=$!

for _ in $(seq 1 100); do
  [[ -S "$runtime_dir/cli.sock" ]] && break
  if ! kill -0 "$vpp_pid" 2>/dev/null; then
    echo "VPP exited before creating its CLI socket" >&2
    cat "$stdout_file" >&2
    exit 1
  fi
  sleep 0.05
done

if [[ ! -S "$runtime_dir/cli.sock" ]]; then
  echo "timed out waiting for VPP CLI socket" >&2
  cat "$stdout_file" >&2
  exit 1
fi

cli=("$vppctl_bin" -s "$runtime_dir/cli.sock")
vat2=("$vat2_bin" -s "$api_prefix" -p "$vat2_plugin_dir")

"${cli[@]}" uet enable
create_reply=$("${vat2[@]}" uet_svm_create \
  "{\"segment_name\":\"$segment_name\",\"queue_depth\":$queue_depth}")
printf '%s\n' "$create_reply"
grep -Eq '"retval":[[:space:]]*0' <<<"$create_reply"

status=$("${cli[@]}" show uet | tr -d '\r')
printf '%s\n' "$status"
grep -q '^state enabled$' <<<"$status"
grep -q '^owner-worker .* (1)$' <<<"$status"
grep -q '^svm-state attached$' <<<"$status"
grep -q "^svm-segment $segment_name$" <<<"$status"
grep -q "^svm-abi ${svm_abi_major}\\.${svm_abi_minor}$" <<<"$status"
grep -q "^svm-queue-depth $queue_depth$" <<<"$status"
grep -q "^svm-dma-slot-count $queue_depth$" <<<"$status"
grep -Eq '^svm-dma-buffer-data-size [1-9][0-9]*$' <<<"$status"
grep -Eq '^svm-dma-map-size [1-9][0-9]*$' <<<"$status"
grep -q "^svm-dma-socket $runtime_dir/uet-dma.sock$" <<<"$status"
grep -q '^tx-ip4-table 0$' <<<"$status"
grep -q '^tx-ip6-table 0$' <<<"$status"

"${cli[@]}" loopback create
"${cli[@]}" set interface state loop0 up
"${cli[@]}" set interface ip address loop0 198.18.0.1/24
"${cli[@]}" "packet-generator new {
  name uet-rx
  limit $rx_packet_count
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
"${cli[@]}" "packet-generator new {
  name uet-crash-rx
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

library_path="$plugin_build_dir/lib:$vpp_prefix/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
env LD_LIBRARY_PATH="$library_path" \
  "$negative_client_bin" "$segment_name" "$runtime_dir/uet-dma.sock" "$rx_packet_count" \
  >"$runtime_dir/negative-client.log" 2>&1 &
negative_pid=$!
for _ in $(seq 1 100); do
  if "${cli[@]}" show uet | tr -d '\r' | grep -q '^provider-ready yes$'; then
    break
  fi
  if ! kill -0 "$negative_pid" 2>/dev/null; then
    cat "$runtime_dir/negative-client.log" >&2
    exit 1
  fi
  sleep 0.01
done
if ! "${cli[@]}" show uet | tr -d '\r' | grep -q '^provider-ready yes$'; then
  echo "negative client did not become ready" >&2
  exit 1
fi
LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name" expect-busy
"${cli[@]}" show uet | tr -d '\r' | grep -q "^provider-owner-pid $negative_pid$"
"${cli[@]}" packet-generator enable-stream uet-rx
if ! wait "$negative_pid"; then
  negative_pid=
  cat "$runtime_dir/negative-client.log" >&2
  exit 1
fi
negative_pid=
negative_client_output=$(<"$runtime_dir/negative-client.log")
printf '%s\n' "$negative_client_output"
grep -q "UET SPSC negative smoke passed: ownership checks, $rx_packet_count RX packets, and $queue_depth-entry completion ring" \
  <<<"$negative_client_output"

status=$("${cli[@]}" show uet | tr -d '\r')
printf '%s\n' "$status"
grep -q "^tx-requests $((queue_depth + 1))$" <<<"$status"
grep -q "^invalid-requests $((queue_depth + 1))$" <<<"$status"
grep -q '^tx-packets 0$' <<<"$status"
grep -q "^tx-completions $((queue_depth + 1))$" <<<"$status"
grep -q "^rx-udp4-packets $rx_packet_count$" <<<"$status"
grep -q "^rx-delivered $rx_packet_count$" <<<"$status"
grep -q "^rx-releases $rx_packet_count$" <<<"$status"
grep -q '^rx-invalid-releases 0$' <<<"$status"
grep -q '^rx-outstanding 0$' <<<"$status"
grep -q '^dma-authorized-clients 1$' <<<"$status"
grep -q '^dma-rejected-clients 0$' <<<"$status"
grep -q '^tx-pending 0$' <<<"$status"
grep -q '^tx-completions-pending 0$' <<<"$status"
grep -q '^provider-owner-pid 0$' <<<"$status"

LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name" expect-double-busy
LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name" expect-existing-heap
LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name" expect-success
"${cli[@]}" show uet | tr -d '\r' | grep -q '^provider-owner-pid 0$'

"${cli[@]}" clear uet counters

"$dma_auth_probe_bin" "$runtime_dir/uet-dma.sock"

"${cli[@]}" ip table add "$ip4_table_id"
"${cli[@]}" ip6 table add "$ip6_table_id"
fib_reply=$("${vat2[@]}" uet_tx_fib_set \
  "{\"ip4_table_id\":$ip4_table_id,\"ip6_table_id\":$ip6_table_id}")
printf '%s\n' "$fib_reply"
grep -Eq '"retval":[[:space:]]*0' <<<"$fib_reply"

tx_client_output=$(LD_LIBRARY_PATH="$plugin_build_dir/lib:$vpp_prefix/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$tx_client_bin" "$segment_name" "$tx_packet_count" "$runtime_dir/uet-dma.sock")
printf '%s\n' "$tx_client_output"
grep -q "UET routed TX smoke passed: $tx_packet_count packets" <<<"$tx_client_output"

status=$("${cli[@]}" show uet | tr -d '\r')
printf '%s\n' "$status"
grep -q "^tx-requests $tx_packet_count$" <<<"$status"
grep -q '^invalid-requests 0$' <<<"$status"
grep -q "^tx-ip4-table $ip4_table_id$" <<<"$status"
grep -q "^tx-ip6-table $ip6_table_id$" <<<"$status"
grep -q "^tx-packets $tx_packet_count$" <<<"$status"
grep -q "^tx-completions $tx_packet_count$" <<<"$status"
grep -q '^dma-authorized-clients 1$' <<<"$status"
grep -q '^dma-rejected-clients 1$' <<<"$status"
grep -q '^tx-pending 0$' <<<"$status"
grep -q '^tx-completions-pending 0$' <<<"$status"

env LD_LIBRARY_PATH="$library_path" \
  "$owner_probe_bin" "$segment_name" hold-dma "$runtime_dir/uet-dma.sock" \
  >"$runtime_dir/owner-probe.log" 2>&1 &
owner_probe_pid=$!
for _ in $(seq 1 100); do
  grep -q '^owner ready:' "$runtime_dir/owner-probe.log" 2>/dev/null && break
  if ! kill -0 "$owner_probe_pid" 2>/dev/null; then
    cat "$runtime_dir/owner-probe.log" >&2
    exit 1
  fi
  sleep 0.01
done
grep -q '^owner ready:' "$runtime_dir/owner-probe.log"
"${cli[@]}" show uet | tr -d '\r' | grep -q "^provider-owner-pid $owner_probe_pid$"
"${cli[@]}" packet-generator enable-stream uet-crash-rx
for _ in $(seq 1 100); do
  "${cli[@]}" show uet | tr -d '\r' | grep -q '^rx-outstanding 1$' && break
  sleep 0.01
done
"${cli[@]}" show uet | tr -d '\r' | grep -q '^rx-outstanding 1$'
busy_delete_reply=$("${vat2[@]}" uet_svm_delete "{\"segment_name\":\"$segment_name\"}")
printf '%s\n' "$busy_delete_reply"
grep -Eq '"retval":[[:space:]]*-[1-9][0-9]*' <<<"$busy_delete_reply"
"${cli[@]}" show uet | tr -d '\r' | grep -q '^svm-state attached$'
dead_owner_pid=$owner_probe_pid
kill -KILL "$owner_probe_pid"
wait "$owner_probe_pid" 2>/dev/null || true
owner_probe_pid=
LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name" expect-owner-dead
"${cli[@]}" show uet | tr -d '\r' | grep -q "^provider-owner-pid $dead_owner_pid$"

delete_reply=$("${vat2[@]}" uet_svm_delete "{\"segment_name\":\"$segment_name\"}")
printf '%s\n' "$delete_reply"
grep -Eq '"retval":[[:space:]]*0' <<<"$delete_reply"
"${cli[@]}" show uet | tr -d '\r' | grep -q '^svm-state detached$'

create_reply=$("${vat2[@]}" uet_svm_create \
  "{\"segment_name\":\"$segment_name\",\"queue_depth\":$queue_depth}")
printf '%s\n' "$create_reply"
grep -Eq '"retval":[[:space:]]*0' <<<"$create_reply"
LD_LIBRARY_PATH="$library_path" "$owner_probe_bin" "$segment_name" expect-success
"${cli[@]}" show uet | tr -d '\r' | grep -q '^provider-owner-pid 0$'
delete_reply=$("${vat2[@]}" uet_svm_delete "{\"segment_name\":\"$segment_name\"}")
printf '%s\n' "$delete_reply"
grep -Eq '"retval":[[:space:]]*0' <<<"$delete_reply"
"${cli[@]}" uet disable

echo "UET SPSC channel smoke test passed; logs: $runtime_dir"
