# UET external libfabric provider

`libuet-fi.so` is an external libfabric provider named `uet`. It lets an
application use the normal libfabric API while choosing the existing UET NIC
backend at runtime. There is no backend-specific provider library:

| `UET_NIC_SHIM` | Engine loaded by `libuet-fi.so` |
| --- | --- |
| unset or `rawsock` | `libuet_fabric.so` |
| `xdp` or `af_xdp` | `libxdpuet.so` |

`UET_ENGINE_LIBRARY=/absolute/path/to/library.so` can override this mapping for
development. The engine is loaded when `fi_fabric()` is called, not during
`fi_getinfo()`, so provider discovery does not acquire a NIC.

## API scope

The initial provider implements:

- `FI_EP_RDM` with `FI_MSG` and `FI_TAGGED` send/receive operations;
- `FI_AV_TABLE`, including lookup and removal;
- context, message, data, and tagged completion-queue formats with manual
  progress;
- endpoint-bound provider-key memory regions (`FI_MR_ENDPOINT |
  FI_MR_PROV_KEY`);
- source-name exchange through `fi_getname()`; and
- a minimal event queue for RDM applications that allocate an EQ as part of a
  common setup path.

The provider reports `FI_THREAD_SAFE`. Control operations are serialized per
domain, AV reads use an RW lock, and CQ reads use a per-CQ mutex. Data
operations retain the existing engine's per-endpoint synchronization; there is
no provider-wide datapath mutex. The current engine lifecycle permits one
active domain per fabric.

RMA, atomics, inject operations, counters, scalable endpoints, shared contexts,
and connection-oriented endpoints are not advertised yet. The UET engine has
some of these operations internally, but they must not be advertised until
their libfabric objects, completion semantics, and error paths are implemented
and tested at this provider boundary.

## Build

With a built libfabric tree in `../libfabric`:

```sh
make provider
make provider-smoke
```

This builds `libuet-fi.so`, `libuet_fabric.so`, and
`uet_provider_smoke`. Use `make provider-xdp` to build the AF_XDP engine as
well.

The provider itself links only to libfabric, pthreads, and `libdl`. Backend
dependencies remain in their engine libraries.

## Run an unchanged libfabric application

Point libfabric at the external provider, make the selected engine visible to
the dynamic loader, and select provider `uet` in the application's normal way:

```sh
export FI_PROVIDER_PATH=$PWD
export LD_LIBRARY_PATH=$PWD:../libfabric/src/.libs
export UET_NIC_SHIM=rawsock
export UET_IFNAME=ens4f0np0

../libfabric/util/fi_info -p uet
../libfabric/util/fi_pingpong -p uet -e rdm -m msg
```

The peer runs the same application and provider. Its backend may be selected
independently.

## Tests

The backend-neutral lifecycle test exercises fabric, domain, AV, CQ, endpoint,
MR, enable, `fi_getname()`, and ordered teardown:

```sh
sudo env \
  FI_PROVIDER_PATH=$PWD \
  LD_LIBRARY_PATH=$PWD:../libfabric/src/.libs \
  UET_NIC_SHIM=rawsock \
  UET_IFNAME=ens4f0np0 \
  ./uet_provider_smoke 192.0.2.1
```

Functional validation also uses the unmodified libfabric `fi_pingpong`
utility. MSG and TAGGED transfers have been validated over the raw-socket
engine and MSG over AF_XDP.

## Zero-copy boundary

The provider layer forwards application buffer pointers and IOVs to the UET
engine without adding a provider-side staging copy. This is not yet
end-to-end zero copy:

- the current AF_XDP engine copies between application data and its UMEM;
- the raw-socket engine serializes TX into a packet buffer and delivers RX
  payload into the application's posted receive buffer.

End-to-end zero copy requires a backend able to register or allocate compatible
application buffers and preserve their ownership through TX, RX, completion,
and cancellation. That can remain a single `uet` provider and does not require
application source changes for applications that already follow the relevant
libfabric MR allocation and registration contract.
