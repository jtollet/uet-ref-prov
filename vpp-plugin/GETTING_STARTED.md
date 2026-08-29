# Getting started with the UET VPP dataplane

This guide builds a known-compatible VPP from source and validates pull
request #130 through an unmodified libfabric application. The validation uses
two Linux network namespaces and VPP AF_PACKET interfaces connected by a
temporary `veth` pair. It does not require a physical NIC, DPDK device binding,
VFs, or XDP.

The tested path is:

```text
fi_pingpong client -> libfabric -> UET provider -> SSVM -> VPP A
    -> AF_PACKET -> veth -> AF_PACKET -> VPP B -> SSVM
    -> UET provider -> libfabric -> fi_pingpong server
```

The namespaces are not required by the UET plugin. They isolate the two VPP
instances and their temporary Linux interfaces from the host network. A second
`veth` pair carries only the TCP control connection used internally by
`fi_pingpong`; UET packets use the AF_PACKET pair.

## Prerequisites

- An x86-64 Linux host supported by VPP, with at least two logical CPUs.
- `sudo` access for installing VPP build dependencies and creating temporary
  network namespaces and `veth` interfaces.
- Git and Internet access to the VPP, libfabric, and UET repositories.

The functional test runs one VPP worker in each VPP instance. Sharing CPUs is
acceptable for this functional test; use dedicated workers and NIC queues only
for performance measurements.

## 1. Clone and build VPP

Start from an empty working directory. PR #130 was validated with VPP commit
`a4b80adfcf792ba8a59ef0f3a9687842333269dd`. Use this pinned revision for the
first test so that the runtime, development files, and external plugin have the
same API and ABI.

```sh
mkdir uet-vpp-evaluation
cd uet-vpp-evaluation
export UET_EVAL_ROOT=$PWD

git clone https://gerrit.fd.io/r/vpp
git -C vpp checkout a4b80adfcf792ba8a59ef0f3a9687842333269dd

make -C vpp UNATTENDED=yes install-dep

cmake -S vpp/src -B vpp-build-uet \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$UET_EVAL_ROOT/vpp-install-uet" \
  -DVPP_PLUGINS=af_packet \
  -DVPP_DRIVERS=none \
  -DVPP_CRYPTO_ENGINES=none \
  -DVPP_MARCH_VARIANT_X86_64_V3=OFF \
  -DVPP_MARCH_VARIANT_X86_64_V4=OFF
cmake --build vpp-build-uet --parallel "$(nproc)"
cmake --install vpp-build-uet

export UET_VPP_PREFIX="$UET_EVAL_ROOT/vpp-install-uet"
export UET_VPP_CMAKE_DIR="$(dirname "$(find "$UET_VPP_PREFIX" \
  -name VPPConfig.cmake -print -quit)")"
"$UET_VPP_PREFIX/bin/vpp" --version
```

This deliberately small build contains the VPP core and AF_PACKET plugin needed
by the functional test. It omits optional PCI drivers, crypto engines, and
DPDK. The `V3` and `V4` options disable additional copies of performance-critical
code compiled for newer x86-64 instruction sets; the baseline x86-64 code is
still built. This reduces functional-test build time and is not a plugin
requirement. Keep the variants enabled in a normal production or performance
build.

The local install remains under the evaluation directory; it does not modify
`/usr`, `/opt`, or an existing VPP installation.

## 2. Clone and build libfabric

PR #130 was validated with libfabric 1.20.1.

```sh
cd "$UET_EVAL_ROOT"
git clone --branch v1.20.1 --depth 1 \
  https://github.com/ofiwg/libfabric.git

cd libfabric
./autogen.sh
./configure --disable-static --enable-only --enable-sockets=yes
make -j"$(nproc)"
```

No libfabric installation is required. The provider and test use the library
and `fi_pingpong` directly from this build tree. The lightweight sockets
provider is enabled only because a libfabric build requires at least one
built-in provider; the end-to-end test explicitly selects the external `uet`
provider.

## 3. Check out PR #130

Repository access may require the credentials supplied to UEC working-group
members.

```sh
cd "$UET_EVAL_ROOT"
git clone https://github.com/ultraethernet/uet-ref-prov.git
git -C uet-ref-prov fetch origin pull/130/head:pr-130
git -C uet-ref-prov switch pr-130
```

## 4. Build the external VPP plugin and UET provider

```sh
cd "$UET_EVAL_ROOT/uet-ref-prov"

cmake -S vpp-plugin -B build/vpp-plugin \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$UET_VPP_PREFIX" \
  -DVPP_DIR="$UET_VPP_CMAKE_DIR"
cmake --build build/vpp-plugin --parallel

make -j"$(nproc)" \
  LIBFABRIC="$UET_EVAL_ROOT/libfabric" \
  VPP_PLUGIN_BUILD="$PWD/build/vpp-plugin" \
  provider-vpp provider-smoke
```

This produces the following relevant components:

| Component | Role |
| --- | --- |
| `libuet-fi.so` | External libfabric provider selected as provider `uet`. |
| `libvppuet.so` | UET reference engine using the VPP client API. |
| `libuet_vpp_client.so` | Provider-facing SSVM and buffer-exchange API. |
| `uet_plugin.so` | Out-of-tree VPP dataplane plugin. |

The VPP source tree remains unmodified; PR #130 is built as an external plugin
against the SDK produced in step 1.

## 5. Run the AF_PACKET end-to-end test

From the UET repository:

```sh
sudo ./vpp-plugin/tests/af_packet_pingpong.sh \
  "$UET_VPP_PREFIX" \
  "$UET_EVAL_ROOT/libfabric"
```

The privileged wrapper provisions, controls, and removes the temporary network
namespaces and `veth` interfaces. When the script is invoked through `sudo`,
both VPP instances run as the invoking user with only `CAP_NET_RAW`,
`CAP_IPC_LOCK`, and `CAP_SYS_NICE`; both `fi_pingpong` processes run as that
user without Linux capabilities. A completely unprivileged invocation would
require the host to permit unprivileged user and network namespaces.

The script performs the following operations automatically:

1. Creates two temporary network namespaces.
2. Creates a data `veth` pair and attaches one end to each VPP through
   AF_PACKET.
3. Creates a separate control `veth` pair for the TCP coordination performed
   by `fi_pingpong`.
4. Starts two isolated VPP instances, each with one worker, one UET SSVM, and
   one AF_PACKET interface.
5. Runs the unmodified libfabric `fi_pingpong` utility with 64-byte messages
   over `FI_EP_RDM` and `FI_MSG`.
6. Prints the UET and interface state from both VPP instances.
7. Stops the processes and removes both namespaces and all four temporary
   interfaces, including after a failure.

A successful run ends with:

```text
UET VPP AF_PACKET fi_pingpong test passed
```

The script preserves its logs under the `/tmp/uet-vpp-af-packet.*` directory
printed at exit. On failure, inspect `vpp-a.log`, `vpp-b.log`, `server.log`,
and `client.log` in that directory.

### UET CLI quick reference

The script configures the plugin through the VPP CLI. These are dataplane
management commands; they do not move UET transport termination into VPP.

VPP listens for CLI clients on the Unix socket configured by `cli-listen` in
its startup file. Start an interactive client by passing that socket to
`vppctl`, or append a command for a one-shot query:

```sh
"$UET_VPP_PREFIX/bin/vppctl" -s /path/to/cli.sock
"$UET_VPP_PREFIX/bin/vppctl" -s /path/to/cli.sock show uet
```

The test normally controls both instances automatically and stops them as soon
as validation finishes. To inspect them interactively, ask the script to pause
after their interfaces, FIBs, and SSVM segments have been configured:

```sh
sudo UET_VPP_INSPECT=1 ./vpp-plugin/tests/af_packet_pingpong.sh \
  "$UET_VPP_PREFIX" \
  "$UET_EVAL_ROOT/libfabric"
```

The script prints the exact `vppctl` command for VPP A and VPP B. Run either
command from another terminal; no network-namespace command or additional
`sudo` is needed because these are filesystem Unix sockets owned by the
invoking user. Press Enter in the first terminal to run `fi_pingpong` and let
the script clean up both instances.

| Command | Purpose |
| --- | --- |
| `uet enable` | Enables the UET VPP graph nodes and local-delivery hooks. The plugin requires at least one VPP worker. |
| `uet disable` | Disables packet processing without deleting an existing SSVM segment. |
| `uet svm create name <name> [queue-size <8-4096>]` | Creates a VPP-owned shared-memory application segment, with one channel per VPP worker. `<name>` must match the provider's `UET_VPP_SEGMENT`. |
| `uet svm delete [name <name>]` | Deletes the named segment. The name is required when more than one application segment exists. |
| `uet tx fib table <id>` | Selects the IPv4 and IPv6 FIB used for provider-originated TX. Table 0 is the default. Separate `ip4-table` and `ip6-table` values are also supported. |
| `uet rx placement current-worker` | Delivers RX on the VPP worker that received the packet. This is the default and preserves NIC RSS placement. |
| `uet rx placement entropy-handoff` | Re-hashes UET entropy and hands packets to a selected worker when the input device cannot provide suitable RSS placement. |
| `show uet` | Shows enablement, segment/application state, worker attachment, queue state, endpoint registrations, and TX/RX/error counters. |
| `clear uet counters` | Clears the plugin counters. |

VPP exposes the syntax compiled into the plugin directly:

```text
help uet
help uet svm create
help uet tx fib
help uet rx placement
```

The commands `create host-interface`, `set interface state`, `set interface ip
address`, `show interface`, `show errors`, and `trace` are standard VPP CLI
commands. The more detailed architecture and operations reference is in
[`README.md`](README.md#vpp-operations).

## 6. Optional internal validation

After the end-to-end test succeeds, the existing multi-worker test exercises
two concurrent provider processes, endpoint ownership and collision recovery,
and exact RX demultiplexing for the supported UET packet forms:

```sh
sudo vpp-plugin/tests/smoke-multiworker.sh \
  "$UET_VPP_PREFIX" build/vpp-plugin 0 1,2
```

This internal test uses VPP's packet generator to target specific error and
demultiplexing paths. It complements the AF_PACKET end-to-end test; it is not
the primary Getting Started datapath.

## Physical interfaces and performance

The virtual AF_PACKET test validates the complete UET/VPP software path without
modifying physical interfaces. Physical-NIC validation should be documented
separately because queue allocation, RSS, NUMA placement, hugepages, and driver
binding are platform-specific and can disrupt connectivity. Do not interpret
AF_PACKET/veth results as physical-NIC performance measurements.
