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
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
runtime_dir=$(mktemp -d /tmp/uet-vpp-smoke.XXXXXX)
config_file="$runtime_dir/startup.conf"
stdout_file="$runtime_dir/stdout.log"
api_prefix="uet-thread-$$"
vpp_pid=

cleanup()
{
  if [[ -n "$vpp_pid" ]] && kill -0 "$vpp_pid" 2>/dev/null; then
    kill "$vpp_pid"
    wait "$vpp_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for required in "$vpp_bin" "$vppctl_bin" "$vat2_bin" \
  "$plugin_dir/uet_plugin.so" "$vat2_plugin_dir/uet_test_plugin_uet_plugin.so"; do
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

unexpected_output=$("${cli[@]}" uet enable unexpected 2>&1 || true)
grep -Eq '(unexpected|unknown) input' <<<"$unexpected_output"
unexpected_output=$("${cli[@]}" show uet unexpected 2>&1 || true)
grep -Eq '(unexpected|unknown) input' <<<"$unexpected_output"
unexpected_output=$("${cli[@]}" uet svm delete unexpected 2>&1 || true)
grep -Eq '(unexpected|unknown) input' <<<"$unexpected_output"

"${cli[@]}" show version
"${cli[@]}" show threads
"${cli[@]}" show plugins | grep -F uet_plugin.so
"${cli[@]}" uet enable
sleep 0.2
status=$("${cli[@]}" show uet | tr -d '\r')
printf '%s\n' "$status"

grep -q '^state enabled$' <<<"$status"
grep -q '^owner-worker .* (1)$' <<<"$status"
grep -q '^tx-ip4-table 0$' <<<"$status"
grep -q '^tx-ip6-table 0$' <<<"$status"
poll_calls=$(awk '/^poll-calls / { print $2 }' <<<"$status")
if [[ -z "$poll_calls" || "$poll_calls" -eq 0 ]]; then
  echo "UET worker did not poll" >&2
  exit 1
fi

api_details=$("$vat2_bin" -s "$api_prefix" -p "$vat2_plugin_dir" \
  uet_worker_dump '{}')
printf '%s\n' "$api_details"
grep -q '"worker_index"[[:space:]]*:[[:space:]]*0' <<<"$api_details"
grep -q '"thread_index"[[:space:]]*:[[:space:]]*1' <<<"$api_details"
grep -q '"enabled"[[:space:]]*:[[:space:]]*true' <<<"$api_details"
grep -q '"tx_configured"[[:space:]]*:[[:space:]]*true' <<<"$api_details"
grep -q '"tx_ip4_table_id"[[:space:]]*:[[:space:]]*0' <<<"$api_details"
grep -q '"tx_ip6_table_id"[[:space:]]*:[[:space:]]*0' <<<"$api_details"

log_output=$("${cli[@]}" show log | tr -d '\r')
printf '%s\n' "$log_output"
grep -q 'uet.*dataplane enabled with 1 workers' <<<"$log_output"

"${cli[@]}" clear uet counters
cleared_status=$("${cli[@]}" show uet | tr -d '\r')
grep -q '^tx-requests 0$' <<<"$cleared_status"
grep -q '^invalid-requests 0$' <<<"$cleared_status"

"${cli[@]}" uet disable
"${cli[@]}" show uet | tr -d '\r' | grep -q '^state disabled$'

echo "UET one-worker smoke test passed; logs: $runtime_dir"
