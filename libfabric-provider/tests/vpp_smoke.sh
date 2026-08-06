#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 <vpp-prefix> <plugin-build-dir> <provider-dir> <libfabric-build-dir>" >&2
  exit 2
fi

vpp_prefix=$1
plugin_build_dir=$2
provider_dir=$3
libfabric_dir=$4
vpp_bin="$vpp_prefix/bin/vpp"
vat2_bin="$vpp_prefix/bin/vat2"
plugin_dir="$plugin_build_dir/lib/vpp_plugins"
vat2_plugin_dir="$plugin_build_dir/lib/vat2_plugins"
provider_smoke="$provider_dir/uet_provider_smoke"
runtime_dir=$(mktemp -d /tmp/uet-fi-vpp.XXXXXX)
config_file="$runtime_dir/startup.conf"
stdout_file="$runtime_dir/stdout.log"
api_prefix="uet-fi-$$"
segment_name="uet-fi-$$"
main_core=${UET_VPP_MAIN_CORE:-2}
worker_cores=${UET_VPP_WORKER_CORES:-${UET_VPP_WORKER_CORE:-3}}
IFS=, read -r -a worker_core_array <<<"$worker_cores"
worker_count=${#worker_core_array[@]}
vpp_pid=

cleanup()
{
  if [[ -n "$vpp_pid" ]] && kill -0 "$vpp_pid" 2>/dev/null; then
    kill "$vpp_pid"
    wait "$vpp_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for required in \
  "$vpp_bin" \
  "$vat2_bin" \
  "$plugin_dir/uet_plugin.so" \
  "$vat2_plugin_dir/uet_test_plugin_uet_plugin.so" \
  "$plugin_build_dir/lib/libuet_vpp_client.so" \
  "$provider_dir/libuet-fi.so" \
  "$provider_dir/libvppuet.so" \
  "$provider_smoke"; do
  if [[ ! -e "$required" ]]; then
    echo "missing required file: $required" >&2
    exit 1
  fi
done

if (( worker_count == 1 )); then
  sed \
    -e "s|@RUNTIME_DIR@|$runtime_dir|g" \
    -e "s|@API_PREFIX@|$api_prefix|g" \
    -e "s|@PLUGIN_DIR@|$plugin_dir|g" \
    -e "s|@MAIN_CORE@|$main_core|g" \
    -e "s|@WORKER_CORE@|$worker_cores|g" \
    "$provider_dir/vpp-plugin/tests/startup-one-worker.conf.in" >"$config_file"
else
  sed \
    -e "s|@RUNTIME_DIR@|$runtime_dir|g" \
    -e "s|@API_PREFIX@|$api_prefix|g" \
    -e "s|@PLUGIN_DIR@|$plugin_dir|g" \
    -e "s|@MAIN_CORE@|$main_core|g" \
    -e "s|@WORKER_STANZA@|corelist-workers $worker_cores|g" \
    -e "s|@BUFFERS_PER_NUMA@|32768|g" \
    "$provider_dir/vpp-plugin/tests/startup-worker-contract.conf.in" >"$config_file"
fi

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

"$vpp_prefix/bin/vppctl" -s "$runtime_dir/cli.sock" uet enable
create_reply=$("$vat2_bin" -s "$api_prefix" -p "$vat2_plugin_dir" \
  uet_svm_create "{\"segment_name\":\"$segment_name\",\"queue_depth\":256}")
grep -Eq '"retval":[[:space:]]*0' <<<"$create_reply"

library_path="$provider_dir:$plugin_build_dir/lib"
library_path+=":$vpp_prefix/lib/x86_64-linux-gnu:$libfabric_dir/src/.libs"
env \
  FI_PROVIDER_PATH="$provider_dir" \
  LD_LIBRARY_PATH="$library_path" \
  UET_NIC_SHIM=vpp \
  UET_IFNAME=vpp0 \
  UET_VPP_SEGMENT="$segment_name" \
  UET_VPP_DMA_SOCKET="$runtime_dir/uet-dma.sock" \
  UET_VPP_IPV4_ADDR=198.18.0.1 \
  timeout 30s "$provider_smoke" 198.18.0.1

status=$("$vpp_prefix/bin/vppctl" -s "$runtime_dir/cli.sock" show uet | tr -d '\r')
grep -q "^svm-channels $worker_count$" <<<"$status"
grep -q "^dma-authorized-clients $worker_count$" <<<"$status"
for ((worker = 0; worker < worker_count; worker++)); do
  grep -q "^worker-$worker-owner-pid 0$" <<<"$status"
done

echo "UET libfabric provider VPP $worker_count-channel smoke test passed; logs: $runtime_dir"
