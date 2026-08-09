// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rdma/fi_errno.h>

#include "uet_fi.h"

static const char *uet_fi_engine_library(void)
{
	const char *backend;
	const char *override;

	override = getenv("UET_ENGINE_LIBRARY");
	if (override && override[0])
		return override;

	backend = getenv("UET_NIC_SHIM");
	if (!backend || !backend[0] || !strcmp(backend, "rawsock"))
		return "libuet_fabric.so";

	if (!strcmp(backend, "xdp") || !strcmp(backend, "af_xdp"))
		return "libxdpuet.so";

	return NULL;
}

static int uet_fi_engine_load_symbol(struct uet_fi_engine *engine,
				     void **target, const char *name)
{
	const char *error;

	dlerror();
	*target = dlsym(engine->dl_handle, name);
	error = dlerror();
	if (!error)
		return FI_SUCCESS;

	fprintf(stderr, "uet provider: %s is missing %s: %s\n",
		engine->name, name, error);
	return -FI_ENOSYS;
}

#define UET_FI_LOAD_SYM(engine, field, symbol) \
	uet_fi_engine_load_symbol((engine), (void **)&(engine)->field, symbol)

int uet_fi_engine_load(struct uet_fi_engine *engine)
{
	const char *library;

	library = uet_fi_engine_library();
	if (!library) {
		fprintf(stderr, "uet provider: invalid UET_NIC_SHIM\n");
		return -FI_EINVAL;
	}

	memset(engine, 0, sizeof(*engine));
	engine->name = library;
	engine->dl_handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	if (!engine->dl_handle) {
		fprintf(stderr, "uet provider: cannot load %s: %s\n",
			library, dlerror());
		return -FI_ENODATA;
	}

	if (UET_FI_LOAD_SYM(engine, initialize, "uet_initialize") ||
	    UET_FI_LOAD_SYM(engine, finalize, "uet_finalize") ||
	    UET_FI_LOAD_SYM(engine, getinfo, "uet_getinfo") ||
	    UET_FI_LOAD_SYM(engine, domain, "uet_domain") ||
	    UET_FI_LOAD_SYM(engine, domain_close, "uet_domain_close") ||
	    UET_FI_LOAD_SYM(engine, endpoint, "uet_endpoint") ||
	    UET_FI_LOAD_SYM(engine, getname, "uet_getname") ||
	    UET_FI_LOAD_SYM(engine, ep_bind_cq, "uet_ep_bind_cq") ||
	    UET_FI_LOAD_SYM(engine, ep_bind_mr, "uet_ep_bind_mr") ||
	    UET_FI_LOAD_SYM(engine, ep_enable, "uet_ep_enable") ||
	    UET_FI_LOAD_SYM(engine, ep_control, "uet_ep_control") ||
	    UET_FI_LOAD_SYM(engine, ep_setopt, "uet_ep_setopt") ||
	    UET_FI_LOAD_SYM(engine, ep_close, "uet_ep_close") ||
	    UET_FI_LOAD_SYM(engine, cancel, "uet_cancel") ||
	    UET_FI_LOAD_SYM(engine, cq_read, "uet_cq_read") ||
	    UET_FI_LOAD_SYM(engine, cq_read_src_id, "uet_cq_read_src_id") ||
	    UET_FI_LOAD_SYM(engine, cq_readerr, "uet_cq_readerr") ||
	    UET_FI_LOAD_SYM(engine, cq_close, "uet_cq_close") ||
	    UET_FI_LOAD_SYM(engine, av_insert, "uet_av_insert") ||
	    UET_FI_LOAD_SYM(engine, av_remove, "uet_av_remove") ||
	    UET_FI_LOAD_SYM(engine, mr_reg, "uet_mr_reg") ||
	    UET_FI_LOAD_SYM(engine, mr_regv, "uet_mr_regv") ||
	    UET_FI_LOAD_SYM(engine, mr_key, "uet_mr_key") ||
	    UET_FI_LOAD_SYM(engine, mr_enable, "uet_mr_enable") ||
	    UET_FI_LOAD_SYM(engine, mr_disable, "uet_mr_disable") ||
	    UET_FI_LOAD_SYM(engine, mr_close, "uet_mr_close") ||
	    UET_FI_LOAD_SYM(engine, send, "uet_send") ||
	    UET_FI_LOAD_SYM(engine, sendv, "uet_sendv") ||
	    UET_FI_LOAD_SYM(engine, recv, "uet_recv") ||
	    UET_FI_LOAD_SYM(engine, recvv, "uet_recvv") ||
	    UET_FI_LOAD_SYM(engine, tsend, "uet_tsend") ||
	    UET_FI_LOAD_SYM(engine, tsendv, "uet_tsendv") ||
	    UET_FI_LOAD_SYM(engine, trecv, "uet_trecv") ||
	    UET_FI_LOAD_SYM(engine, trecvv, "uet_trecvv"))
		goto missing_symbol;

	return FI_SUCCESS;

missing_symbol:
	uet_fi_engine_unload(engine);
	return -FI_ENOSYS;
}

void uet_fi_engine_unload(struct uet_fi_engine *engine)
{
	if (engine->dl_handle)
		dlclose(engine->dl_handle);
	memset(engine, 0, sizeof(*engine));
}
