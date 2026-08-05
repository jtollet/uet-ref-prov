#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <vpp-prefix> <plugin-build-dir> [main-core]" >&2
  exit 2
fi

vpp_prefix=$1
plugin_build_dir=$2
main_core=${3:-2}

vpp_bin="$vpp_prefix/bin/vpp"
vppctl_bin="$vpp_prefix/bin/vppctl"
plugin_dir="$plugin_build_dir/lib/vpp_plugins"
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
vpp_pid=

cleanup()
{
  if [[ -n "$vpp_pid" ]] && kill -0 "$vpp_pid" 2>/dev/null; then
    kill "$vpp_pid"
    wait "$vpp_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for required in "$vpp_bin" "$vppctl_bin" "$plugin_dir/uet_plugin.so"; do
  if [[ ! -e "$required" ]]; then
    echo "missing required file: $required" >&2
    exit 1
  fi
done

run_rejection_case()
{
  local label=$1
  local worker_stanza=$2
  local runtime_dir config_file stdout_file api_prefix output state

  runtime_dir=$(mktemp -d "/tmp/uet-vpp-${label}.XXXXXX")
  config_file="$runtime_dir/startup.conf"
  stdout_file="$runtime_dir/stdout.log"
  api_prefix="uet-${label}-$$"

  sed \
    -e "s|@RUNTIME_DIR@|$runtime_dir|g" \
    -e "s|@API_PREFIX@|$api_prefix|g" \
    -e "s|@PLUGIN_DIR@|$plugin_dir|g" \
    -e "s|@MAIN_CORE@|$main_core|g" \
    -e "s|@WORKER_STANZA@|$worker_stanza|g" \
    -e "s|@BUFFERS_PER_NUMA@|${UET_BUFFERS_PER_NUMA:-32768}|g" \
    "$script_dir/startup-worker-contract.conf.in" >"$config_file"

  "$vpp_bin" -c "$config_file" >"$stdout_file" 2>&1 &
  vpp_pid=$!

  for _ in $(seq 1 100); do
    [[ -S "$runtime_dir/cli.sock" ]] && break
    if ! kill -0 "$vpp_pid" 2>/dev/null; then
      echo "VPP exited before creating its CLI socket ($label case)" >&2
      cat "$stdout_file" >&2
      exit 1
    fi
    sleep 0.05
  done

  if [[ ! -S "$runtime_dir/cli.sock" ]]; then
    echo "timed out waiting for VPP CLI socket ($label case)" >&2
    cat "$stdout_file" >&2
    exit 1
  fi

  output=$("$vppctl_bin" -s "$runtime_dir/cli.sock" uet enable 2>&1 || true)
  output=$(tr -d '\r' <<<"$output")
  printf '%s: %s\n' "$label" "$output"
  grep -Fq "requires at least one VPP worker" <<<"$output"

  state=$("$vppctl_bin" -s "$runtime_dir/cli.sock" show uet | tr -d '\r')
  grep -q '^state disabled$' <<<"$state"

  kill "$vpp_pid"
  wait "$vpp_pid" 2>/dev/null || true
  vpp_pid=
}

run_rejection_case zero-workers ""

echo "UET zero-worker rejection test passed"
