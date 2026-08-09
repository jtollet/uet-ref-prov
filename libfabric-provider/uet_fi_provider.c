/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include <rdma/fi_errno.h>
#include <rdma/providers/fi_prov.h>

#include "uet_fi.h"
#include "uet_fi_noop.h"

#define UET_FI_CAPS (FI_MSG | FI_TAGGED | FI_SEND | FI_RECV | \
		     FI_DIRECTED_RECV | FI_LOCAL_COMM | FI_REMOTE_COMM)

static struct fi_provider uet_fi_provider;

static void uet_fi_addr_init(struct uet_addr *addr)
{
	memset(addr, 0, sizeof(*addr));
	addr->ver = UET_ADDR_VERSION;
	addr->flags = UET_ADDR_FEP_CAP_V | UET_ADDR_FA_V |
		      UET_ADDR_PID_ON_FEP_V | UET_ADDR_INDEX_V |
		      UET_ADDR_INITIATOR_V | UET_ADDR_RELATIVE_MODE |
		      UET_ADDR_IPV4 | UET_ADDR_BIG_MSG_SIZE;
	addr->fep_cap = UET_FEP_CAP_AI_FULL | UET_FEP_CAP_HPC;
	addr->pid_on_fep = UET_ADDR_DEF_PID_ON_FEP;
	addr->start_index = UET_ADDR_DEF_INDEX;
	addr->num_indices = 1;
	addr->initiator_id = UET_ADDR_DEF_INITIATOR_ID;
}

static int uet_fi_parse_node(const char *node, struct uet_addr *addr)
{
	struct in_addr in4;
	struct in6_addr in6;

	uet_fi_addr_init(addr);
	if (!node || !node[0])
		return FI_SUCCESS;

	if (inet_pton(AF_INET, node, &in4) == 1) {
		addr->fa.v4 = ntohl(in4.s_addr);
		return FI_SUCCESS;
	}

	if (inet_pton(AF_INET6, node, &in6) == 1) {
		addr->flags &= ~UET_ADDR_IPV4;
		addr->flags |= UET_ADDR_IPV6;
		memcpy(addr->fa.v6, &in6, sizeof(in6));
		return FI_SUCCESS;
	}

	return -FI_ENODATA;
}

static int uet_fi_check_hints(const struct fi_info *hints)
{
	if (!hints)
		return FI_SUCCESS;

	if (hints->caps & ~UET_FI_CAPS)
		return -FI_ENODATA;

	if (hints->addr_format != FI_FORMAT_UNSPEC)
		return -FI_ENODATA;
	if ((hints->src_addr && hints->src_addrlen != sizeof(struct uet_addr)) ||
	    (hints->dest_addr &&
	     hints->dest_addrlen != sizeof(struct uet_addr)))
		return -FI_ENODATA;
	if ((hints->src_addr &&
	     ((struct uet_addr *)hints->src_addr)->ver != UET_ADDR_VERSION) ||
	    (hints->dest_addr &&
	     ((struct uet_addr *)hints->dest_addr)->ver != UET_ADDR_VERSION))
		return -FI_ENODATA;

	if (hints->ep_attr && hints->ep_attr->type != FI_EP_UNSPEC &&
	    hints->ep_attr->type != FI_EP_RDM)
		return -FI_ENODATA;

	if (hints->domain_attr) {
		if (hints->domain_attr->threading != FI_THREAD_UNSPEC &&
		    hints->domain_attr->threading != FI_THREAD_SAFE &&
		    hints->domain_attr->threading != FI_THREAD_FID &&
		    hints->domain_attr->threading != FI_THREAD_ENDPOINT)
			return -FI_ENODATA;
		if (hints->domain_attr->control_progress != FI_PROGRESS_UNSPEC &&
		    hints->domain_attr->control_progress != FI_PROGRESS_MANUAL)
			return -FI_ENODATA;
		if (hints->domain_attr->data_progress != FI_PROGRESS_UNSPEC &&
		    hints->domain_attr->data_progress != FI_PROGRESS_MANUAL)
			return -FI_ENODATA;
		if (hints->domain_attr->av_type != FI_AV_UNSPEC &&
		    hints->domain_attr->av_type != FI_AV_TABLE)
			return -FI_ENODATA;
	}

	return FI_SUCCESS;
}

static int uet_fi_set_names(struct fi_info *info)
{
	info->fabric_attr->name = strdup(UET_FI_FABRIC_NAME);
	info->domain_attr->name = strdup(UET_FI_DOMAIN_NAME);
	/* The libfabric core adds the provider name to returned fi_info. Setting
	 * it here would make the provider look like a utility stack (uet;uet).
	 */
	if (!info->fabric_attr->name || !info->domain_attr->name)
		return -FI_ENOMEM;
	return FI_SUCCESS;
}

static int uet_fi_copy_addr(void **dst, size_t *dstlen, const void *src,
			    size_t srclen)
{
	if (!src)
		return FI_SUCCESS;
	*dst = malloc(srclen);
	if (!*dst)
		return -FI_ENOMEM;
	memcpy(*dst, src, srclen);
	*dstlen = srclen;
	return FI_SUCCESS;
}

static int uet_fi_getinfo(uint32_t version, const char *node,
			  const char *service, uint64_t flags,
			  const struct fi_info *hints, struct fi_info **info_out)
{
	struct fi_info *info;
	struct uet_addr addr;
	void **addr_out;
	size_t *addrlen_out;
	uint64_t caps;
	bool have_node;
	int rc;

	(void)service; /* UET addressing has no port/service component. */

	rc = uet_fi_check_hints(hints);
	if (rc)
		return rc;

	info = fi_allocinfo();
	if (!info)
		return -FI_ENOMEM;

	rc = uet_fi_set_names(info);
	if (rc)
		goto err;

	caps = hints && hints->caps ? hints->caps : UET_FI_CAPS;
	if (caps & (FI_MSG | FI_TAGGED))
		caps |= FI_SEND | FI_RECV;
	info->caps = caps;
	info->mode = hints ? hints->mode & (FI_CONTEXT | FI_CONTEXT2) : 0;
	info->addr_format = FI_FORMAT_UNSPEC;

	info->tx_attr->caps = caps & (FI_MSG | FI_TAGGED | FI_SEND |
				      FI_LOCAL_COMM | FI_REMOTE_COMM);
	info->tx_attr->mode = info->mode;
	info->tx_attr->msg_order = FI_ORDER_SAS;
	info->tx_attr->size = UET_FI_DEFAULT_QUEUE;
	info->tx_attr->iov_limit = UET_FI_IOV_LIMIT;
	info->tx_attr->rma_iov_limit = UET_FI_IOV_LIMIT;
	info->tx_attr->tclass = FI_TC_BEST_EFFORT;

	info->rx_attr->caps = caps & (FI_MSG | FI_TAGGED | FI_RECV |
				      FI_DIRECTED_RECV | FI_LOCAL_COMM |
				      FI_REMOTE_COMM);
	info->rx_attr->mode = info->mode;
	info->rx_attr->msg_order = FI_ORDER_SAS;
	info->rx_attr->size = UET_FI_DEFAULT_QUEUE;
	info->rx_attr->iov_limit = UET_FI_IOV_LIMIT;

	info->ep_attr->type = FI_EP_RDM;
	info->ep_attr->protocol = FI_PROTO_UNSPEC;
	info->ep_attr->protocol_version = 0;
	info->ep_attr->max_msg_size = UET_FI_MAX_MSG_SIZE;
	info->ep_attr->mem_tag_format = UINT64_MAX;
	info->ep_attr->tx_ctx_cnt = 1;
	info->ep_attr->rx_ctx_cnt = 1;

	info->domain_attr->threading = FI_THREAD_SAFE;
	info->domain_attr->control_progress = FI_PROGRESS_MANUAL;
	info->domain_attr->data_progress = FI_PROGRESS_MANUAL;
	info->domain_attr->resource_mgmt = FI_RM_ENABLED;
	info->domain_attr->av_type = FI_AV_TABLE;
	info->domain_attr->mr_mode = FI_MR_ENDPOINT | FI_MR_PROV_KEY;
	info->domain_attr->mr_key_size = sizeof(uint64_t);
	info->domain_attr->cq_cnt = 2;
	info->domain_attr->ep_cnt = 1;
	info->domain_attr->tx_ctx_cnt = 1;
	info->domain_attr->rx_ctx_cnt = 1;
	info->domain_attr->max_ep_tx_ctx = 1;
	info->domain_attr->max_ep_rx_ctx = 1;
	info->domain_attr->mr_iov_limit = UET_FI_IOV_LIMIT;
	info->domain_attr->caps = caps;
	info->domain_attr->mode = info->mode;
	info->domain_attr->mr_cnt = UET_DEF_MR_CNT;
	info->fabric_attr->prov_version = uet_fi_provider.version;
	info->fabric_attr->api_version = version;

	have_node = node && node[0];
	if (!have_node && hints) {
		rc = uet_fi_copy_addr(&info->src_addr, &info->src_addrlen,
				      hints->src_addr, hints->src_addrlen);
		if (rc)
			goto err;
		rc = uet_fi_copy_addr(&info->dest_addr, &info->dest_addrlen,
				      hints->dest_addr, hints->dest_addrlen);
		if (rc)
			goto err;
	}
	if (have_node) {
		rc = uet_fi_parse_node(node, &addr);
		if (rc)
			goto err;

		if (flags & FI_SOURCE) {
			addr_out = &info->src_addr;
			addrlen_out = &info->src_addrlen;
		} else {
			addr_out = &info->dest_addr;
			addrlen_out = &info->dest_addrlen;
		}

		*addr_out = malloc(sizeof(addr));
		if (!*addr_out) {
			rc = -FI_ENOMEM;
			goto err;
		}
		memcpy(*addr_out, &addr, sizeof(addr));
		*addrlen_out = sizeof(addr);
	}

	/* When a destination was supplied, retain its address family as a source
	 * selector. With no node, leave the source unset so the selected engine
	 * can choose from the addresses actually present on its interface.
	 * fi_getname() returns the concrete address after endpoint creation.
	 */
	if (have_node && !(flags & FI_SOURCE)) {
		info->src_addr = malloc(sizeof(addr));
		if (!info->src_addr) {
			rc = -FI_ENOMEM;
			goto err;
		}
		uet_fi_addr_init(info->src_addr);
		if (uet_addr_is_ipv6(&addr)) {
			((struct uet_addr *)info->src_addr)->flags &= ~UET_ADDR_IPV4;
			((struct uet_addr *)info->src_addr)->flags |= UET_ADDR_IPV6;
		}
		info->src_addrlen = sizeof(addr);
	}

	*info_out = info;
	return FI_SUCCESS;

err:
	fi_freeinfo(info);
	return rc;
}

static int uet_fi_fabric_close(struct fid *fid)
{
	struct uet_fi_fabric *fabric;
	int rc;

	fabric = UET_FI_CONTAINER(fid, struct uet_fi_fabric, fabric_fid.fid);
	if (atomic_load_explicit(&fabric->refs, memory_order_acquire))
		return -FI_EBUSY;

	rc = fabric->engine.finalize(fabric->uet);
	if (rc)
		return rc;
	uet_fi_engine_unload(&fabric->engine);
	free(fabric);
	return FI_SUCCESS;
}

static struct fi_ops uet_fi_fabric_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = uet_fi_fabric_close,
	.bind = uet_fi_no_bind,
	.control = uet_fi_no_control,
	.ops_open = uet_fi_no_ops_open,
	.tostr = uet_fi_no_tostr,
	.ops_set = uet_fi_no_ops_set,
};

static struct fi_ops_fabric uet_fi_fabric_ops = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = uet_fi_domain_open,
	.passive_ep = uet_fi_no_passive_ep,
	.eq_open = uet_fi_eq_open,
	.wait_open = uet_fi_no_wait_open,
	.trywait = uet_fi_no_trywait,
	.domain2 = uet_fi_no_domain2,
};

static int uet_fi_fabric_open(struct fi_fabric_attr *attr,
			      struct fid_fabric **fabric_out, void *context)
{
	struct uet_fi_fabric *fabric;
	int rc;

	if (!attr || (attr->name && strcmp(attr->name, UET_FI_FABRIC_NAME)) ||
	    (attr->prov_name && strcmp(attr->prov_name, UET_FI_PROV_NAME)))
		return -FI_ENODATA;

	fabric = calloc(1, sizeof(*fabric));
	if (!fabric)
		return -FI_ENOMEM;

	rc = uet_fi_engine_load(&fabric->engine);
	if (rc)
		goto err_free;

	rc = fabric->engine.initialize(&fabric->uet);
	if (rc)
		goto err_unload;

	fabric->fabric_fid.fid.fclass = FI_CLASS_FABRIC;
	fabric->fabric_fid.fid.context = context;
	fabric->fabric_fid.fid.ops = &uet_fi_fabric_fid_ops;
	fabric->fabric_fid.ops = &uet_fi_fabric_ops;
	fabric->fabric_fid.api_version = attr->api_version;
	atomic_init(&fabric->refs, 0);
	atomic_init(&fabric->domain_open, false);
	*fabric_out = &fabric->fabric_fid;
	return FI_SUCCESS;

err_unload:
	uet_fi_engine_unload(&fabric->engine);
err_free:
	free(fabric);
	return rc;
}

static void uet_fi_cleanup(void)
{
}

static struct fi_provider uet_fi_provider = {
	.version = FI_VERSION(0, 1),
	.fi_version = FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
	.name = UET_FI_PROV_NAME,
	.getinfo = uet_fi_getinfo,
	.fabric = uet_fi_fabric_open,
	.cleanup = uet_fi_cleanup,
};

FI_EXT_INI
{
	return &uet_fi_provider;
}
