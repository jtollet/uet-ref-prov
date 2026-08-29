#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage()
{
  echo "usage: sudo $0 <vpp-prefix> <libfabric-build-dir> [plugin-build-dir]" >&2
}

if [[ $# -lt 2 || $# -gt 3 ]]; then
  usage
  exit 2
fi
if (( EUID != 0 )); then
  echo "this test creates temporary network namespaces; run it with sudo" >&2
  usage
  exit 2
fi

# When invoked through sudo, keep root in the orchestration shell only. VPP
# runs as the invoking user with a small capability set and fi_pingpong runs as
# that user without capabilities.
test_uid=${SUDO_UID:-0}
test_gid=${SUDO_GID:-0}
vpp_user_command=()
application_user_command=()

if (( test_uid != 0 )); then
  if ! command -v setpriv >/dev/null 2>&1; then
    echo "missing required command: setpriv" >&2
    exit 1
  fi
  vpp_user_command=(
    setpriv
    --reuid="$test_uid"
    --regid="$test_gid"
    --init-groups
    --inh-caps=+net_raw,+ipc_lock,+sys_nice
    --ambient-caps=+net_raw,+ipc_lock,+sys_nice
  )
  application_user_command=(
    setpriv
    --reuid="$test_uid"
    --regid="$test_gid"
    --init-groups
  )
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
provider_dir=$(cd "$script_dir/../.." && pwd)
vpp_prefix=$(cd "$1" && pwd)
libfabric_dir=$(cd "$2" && pwd)
plugin_build_dir=${3:-$provider_dir/build/vpp-plugin}
plugin_build_dir=$(cd "$plugin_build_dir" && pwd)

vpp_bin="$vpp_prefix/bin/vpp"
vppctl_bin="$vpp_prefix/bin/vppctl"
fi_pingpong="$libfabric_dir/util/fi_pingpong"
custom_plugin_dir="$plugin_build_dir/lib/vpp_plugins"
runtime_dir=$(mktemp -d /tmp/uet-vpp-af-packet.XXXXXX)
vpp_library_dir=$(dirname "$(find "$vpp_prefix/lib" -maxdepth 2 \
  -name libvppinfra.so -print -quit)")

if [[ "$vpp_library_dir" == "." ]]; then
  echo "could not locate the VPP multiarch library directory under $vpp_prefix/lib" >&2
  exit 1
fi

suffix=$$
namespace_a="uet-vpp-a-$suffix"
namespace_b="uet-vpp-b-$suffix"
data_a="uvda$suffix"
data_b="uvdb$suffix"
control_a="uvca$suffix"
control_b="uvcb$suffix"
segment_a="uet-afp-a-$suffix"
segment_b="uet-afp-b-$suffix"
api_prefix_a="uet-afp-a-$suffix"
api_prefix_b="uet-afp-b-$suffix"

vpp_pid_a=
vpp_pid_b=
server_pid=

cleanup()
{
  for process_id in "$server_pid" "$vpp_pid_a" "$vpp_pid_b"; do
    if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
      kill "$process_id" 2>/dev/null || true
      wait "$process_id" 2>/dev/null || true
    fi
  done
  ip netns delete "$namespace_a" 2>/dev/null || true
  ip netns delete "$namespace_b" 2>/dev/null || true
  echo "logs: $runtime_dir"
}
trap cleanup EXIT

for required in \
  "$vpp_bin" \
  "$vppctl_bin" \
  "$fi_pingpong" \
  "$custom_plugin_dir/uet_plugin.so" \
  "$plugin_build_dir/lib/libuet_vpp_client.so" \
  "$provider_dir/libuet-fi.so" \
  "$provider_dir/libvppuet.so" \
  "$libfabric_dir/src/.libs/libfabric.so"; do
  if [[ ! -e "$required" ]]; then
    echo "missing required file: $required" >&2
    exit 1
  fi
done

for command_name in ip timeout; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "missing required command: $command_name" >&2
    exit 1
  fi
done

cat >"$runtime_dir/vpp-a.conf" <<EOF
unix {
  nodaemon
  runtime-dir $runtime_dir/a
  cli-listen $runtime_dir/a/cli.sock
  log $runtime_dir/vpp-a.log
}
api-segment { prefix $api_prefix_a }
statseg { socket-name $runtime_dir/a/stats.sock }
cpu { workers 1 }
plugins {
  add-path $custom_plugin_dir
  plugin uet_plugin.so { enable }
}
EOF

cat >"$runtime_dir/vpp-b.conf" <<EOF
unix {
  nodaemon
  runtime-dir $runtime_dir/b
  cli-listen $runtime_dir/b/cli.sock
  log $runtime_dir/vpp-b.log
}
api-segment { prefix $api_prefix_b }
statseg { socket-name $runtime_dir/b/stats.sock }
cpu { workers 1 }
plugins {
  add-path $custom_plugin_dir
  plugin uet_plugin.so { enable }
}
EOF

mkdir -p "$runtime_dir/a" "$runtime_dir/b"

if (( test_uid != 0 )); then
  chown -R "$test_uid:$test_gid" "$runtime_dir"
fi

ip netns add "$namespace_a"
ip netns add "$namespace_b"

ip link add "$data_a" type veth peer name "$data_b"
ip link set "$data_a" netns "$namespace_a"
ip link set "$data_b" netns "$namespace_b"
ip -n "$namespace_a" link set "$data_a" name uet-data
ip -n "$namespace_b" link set "$data_b" name uet-data

ip link add "$control_a" type veth peer name "$control_b"
ip link set "$control_a" netns "$namespace_a"
ip link set "$control_b" netns "$namespace_b"
ip -n "$namespace_a" link set "$control_a" name uet-control
ip -n "$namespace_b" link set "$control_b" name uet-control

ip -n "$namespace_a" link set lo up
ip -n "$namespace_b" link set lo up
ip -n "$namespace_a" link set uet-data up
ip -n "$namespace_b" link set uet-data up
ip -n "$namespace_a" link set uet-control up
ip -n "$namespace_b" link set uet-control up
ip -n "$namespace_a" address add 198.18.0.1/30 dev uet-control
ip -n "$namespace_b" address add 198.18.0.2/30 dev uet-control

vpp_library_path="$vpp_library_dir:$vpp_prefix/lib"
ip netns exec "$namespace_a" "${vpp_user_command[@]}" \
  env LD_LIBRARY_PATH="$vpp_library_path" \
  "$vpp_bin" -c "$runtime_dir/vpp-a.conf" >"$runtime_dir/vpp-a.stdout" 2>&1 &
vpp_pid_a=$!
ip netns exec "$namespace_b" "${vpp_user_command[@]}" \
  env LD_LIBRARY_PATH="$vpp_library_path" \
  "$vpp_bin" -c "$runtime_dir/vpp-b.conf" >"$runtime_dir/vpp-b.stdout" 2>&1 &
vpp_pid_b=$!

wait_for_vpp()
{
  local process_id=$1
  local cli_socket=$2
  local stdout_file=$3

  for _ in $(seq 1 200); do
    [[ -S "$cli_socket" ]] && return 0
    if ! kill -0 "$process_id" 2>/dev/null; then
      echo "VPP exited before creating $cli_socket" >&2
      cat "$stdout_file" >&2
      return 1
    fi
    sleep 0.05
  done
  echo "timed out waiting for $cli_socket" >&2
  cat "$stdout_file" >&2
  return 1
}

wait_for_vpp "$vpp_pid_a" "$runtime_dir/a/cli.sock" "$runtime_dir/vpp-a.stdout"
wait_for_vpp "$vpp_pid_b" "$runtime_dir/b/cli.sock" "$runtime_dir/vpp-b.stdout"

cli_a=("$vppctl_bin" -s "$runtime_dir/a/cli.sock")
cli_b=("$vppctl_bin" -s "$runtime_dir/b/cli.sock")

"${cli_a[@]}" create host-interface name uet-data
"${cli_b[@]}" create host-interface name uet-data
"${cli_a[@]}" set interface state host-uet-data up
"${cli_b[@]}" set interface state host-uet-data up
"${cli_a[@]}" set interface ip address host-uet-data 198.18.0.1/30
"${cli_b[@]}" set interface ip address host-uet-data 198.18.0.2/30
"${cli_a[@]}" uet enable
"${cli_b[@]}" uet enable
"${cli_a[@]}" uet svm create name "$segment_a" queue-size 256
"${cli_b[@]}" uet svm create name "$segment_b" queue-size 256

if [[ ${UET_VPP_INSPECT:-0} == 1 ]]; then
  echo "Both VPP instances are ready. From another terminal, run:"
  printf '  %q -s %q\n' "$vppctl_bin" "$runtime_dir/a/cli.sock"
  printf '  %q -s %q\n' "$vppctl_bin" "$runtime_dir/b/cli.sock"
  read -r -p "Press Enter to start fi_pingpong... " _
fi

application_library_path="$provider_dir:$plugin_build_dir/lib"
application_library_path+=":$vpp_library_dir:$vpp_prefix/lib"
application_library_path+=":$libfabric_dir/src/.libs"

run_pingpong()
{
  local network_namespace=$1
  local segment_name=$2
  local dma_socket=$3
  local local_address=$4
  shift 4

  ip netns exec "$network_namespace" "${application_user_command[@]}" env \
    FI_PROVIDER_PATH="$provider_dir" \
    LD_LIBRARY_PATH="$application_library_path" \
    UET_NIC_SHIM=vpp \
    UET_IFNAME=host-uet-data \
    UET_VPP_SEGMENT="$segment_name" \
    UET_VPP_DMA_SOCKET="$dma_socket" \
    UET_VPP_IPV4_ADDR="$local_address" \
    timeout 60s "$fi_pingpong" -p uet -e rdm -m msg -I 100 -S 64 "$@"
}

run_pingpong "$namespace_a" "$segment_a" "$runtime_dir/a/uet-dma.sock" \
  198.18.0.1 >"$runtime_dir/server.log" 2>&1 &
server_pid=$!
sleep 1
if ! kill -0 "$server_pid" 2>/dev/null; then
  echo "fi_pingpong server exited before the client started" >&2
  cat "$runtime_dir/server.log" >&2
  exit 1
fi

set +e
run_pingpong "$namespace_b" "$segment_b" "$runtime_dir/b/uet-dma.sock" \
  198.18.0.2 198.18.0.1 >"$runtime_dir/client.log" 2>&1
client_status=$?
if (( client_status == 0 )); then
  wait "$server_pid"
  server_status=$?
else
  server_status=1
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
fi
server_pid=
set -e

if (( client_status != 0 || server_status != 0 )); then
  echo "fi_pingpong failed: client=$client_status server=$server_status" >&2
  echo "--- server.log ---" >&2
  cat "$runtime_dir/server.log" >&2
  echo "--- client.log ---" >&2
  cat "$runtime_dir/client.log" >&2
  echo "--- VPP A UET state ---" >&2
  "${cli_a[@]}" show uet >&2 || true
  echo "--- VPP B UET state ---" >&2
  "${cli_b[@]}" show uet >&2 || true
  exit 1
fi

cat "$runtime_dir/server.log"
cat "$runtime_dir/client.log"
echo "--- VPP A UET state ---"
"${cli_a[@]}" show uet
echo "--- VPP B UET state ---"
"${cli_b[@]}" show uet
echo "--- VPP A AF_PACKET interface ---"
"${cli_a[@]}" show interface host-uet-data
echo "--- VPP B AF_PACKET interface ---"
"${cli_b[@]}" show interface host-uet-data

echo "UET VPP AF_PACKET fi_pingpong test passed"
