// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#include <stdlib.h>

#include <rdma/fi_errno.h>

#include "uet_fi.h"
#include "uet_fi_noop.h"

static int uet_fi_domain_close(struct fid *fid)
{
	struct uet_fi_domain *domain;
	int rc;

	domain = UET_FI_CONTAINER(fid, struct uet_fi_domain, domain_fid.fid);
	if (atomic_load_explicit(&domain->refs, memory_order_acquire))
		return -FI_EBUSY;

	rc = domain->fabric->engine.domain_close(domain->uet_domain);
	if (rc)
		return rc;

	fi_freeinfo(domain->engine_info);
	pthread_mutex_destroy(&domain->control_lock);
	atomic_store_explicit(&domain->fabric->domain_open, false,
			      memory_order_release);
	atomic_fetch_sub_explicit(&domain->fabric->refs, 1, memory_order_release);
	free(domain);
	return FI_SUCCESS;
}

static struct fi_ops uet_fi_domain_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = uet_fi_domain_close,
	.bind = uet_fi_no_bind,
	.control = uet_fi_no_control,
	.ops_open = uet_fi_no_ops_open,
	.tostr = uet_fi_no_tostr,
	.ops_set = uet_fi_no_ops_set,
};

static struct fi_ops_domain uet_fi_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = uet_fi_av_open,
	.cq_open = uet_fi_cq_open,
	.endpoint = uet_fi_endpoint_open,
	.scalable_ep = uet_fi_no_scalable_ep,
	.cntr_open = uet_fi_no_cntr_open,
	.poll_open = uet_fi_no_poll_open,
	.stx_ctx = uet_fi_no_stx_context,
	.srx_ctx = uet_fi_no_srx_context,
	.query_atomic = uet_fi_no_query_atomic,
	.query_collective = uet_fi_no_query_collective,
	.endpoint2 = uet_fi_no_endpoint2,
};

static struct fi_ops_mr uet_fi_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = uet_fi_mr_reg,
	.regv = uet_fi_mr_regv,
	.regattr = uet_fi_mr_regattr,
};

static void uet_fi_merge_engine_info(struct fi_info *engine_info,
				     const struct fi_info *requested)
{
	engine_info->caps = requested->caps;
	engine_info->mode = requested->mode;
	engine_info->addr_format = FI_FORMAT_UNSPEC;

	engine_info->tx_attr->caps = requested->tx_attr->caps;
	engine_info->tx_attr->mode = requested->tx_attr->mode;
	engine_info->tx_attr->op_flags = requested->tx_attr->op_flags;
	engine_info->tx_attr->msg_order = requested->tx_attr->msg_order;
	engine_info->tx_attr->size = requested->tx_attr->size ?
		requested->tx_attr->size : UET_FI_DEFAULT_QUEUE;
	engine_info->tx_attr->iov_limit = UET_FI_IOV_LIMIT;
	engine_info->tx_attr->rma_iov_limit = UET_FI_IOV_LIMIT;
	engine_info->tx_attr->tclass = requested->tx_attr->tclass;

	engine_info->rx_attr->caps = requested->rx_attr->caps;
	engine_info->rx_attr->mode = requested->rx_attr->mode;
	engine_info->rx_attr->op_flags = requested->rx_attr->op_flags;
	engine_info->rx_attr->msg_order = requested->rx_attr->msg_order;
	engine_info->rx_attr->size = requested->rx_attr->size ?
		requested->rx_attr->size : UET_FI_DEFAULT_QUEUE;
	engine_info->rx_attr->iov_limit = UET_FI_IOV_LIMIT;

	engine_info->ep_attr->type = FI_EP_RDM;
	engine_info->ep_attr->max_msg_size = UET_FI_MAX_MSG_SIZE;
	engine_info->ep_attr->mem_tag_format = UINT64_MAX;
	engine_info->ep_attr->tx_ctx_cnt = 1;
	engine_info->ep_attr->rx_ctx_cnt = 1;

	engine_info->domain_attr->threading = FI_THREAD_SAFE;
	engine_info->domain_attr->control_progress = FI_PROGRESS_MANUAL;
	engine_info->domain_attr->data_progress = FI_PROGRESS_MANUAL;
	engine_info->domain_attr->resource_mgmt = FI_RM_ENABLED;
	engine_info->domain_attr->av_type = FI_AV_TABLE;
	engine_info->domain_attr->mr_mode = FI_MR_ENDPOINT | FI_MR_PROV_KEY;
	engine_info->domain_attr->mr_key_size = sizeof(uint64_t);
	engine_info->domain_attr->mr_iov_limit = UET_FI_IOV_LIMIT;
	engine_info->domain_attr->mr_cnt = requested->domain_attr->mr_cnt ?
		requested->domain_attr->mr_cnt : UET_FI_DEFAULT_MR_CNT;
}

int uet_fi_domain_open(struct fid_fabric *fabric_fid, struct fi_info *info,
		       struct fid_domain **domain_out, void *context)
{
	struct uet_fi_fabric *fabric;
	struct uet_fi_domain *domain;
	struct uet_addr *family = NULL;
	bool expected = false;
	int rc;

	if (!fabric_fid || !info || !domain_out)
		return -FI_EINVAL;
	if (!info->ep_attr || info->ep_attr->type != FI_EP_RDM)
		return -FI_ENODATA;
	if (!info->domain_attr ||
	    !(info->domain_attr->mr_mode & FI_MR_ENDPOINT))
		return -FI_ENODATA;

	fabric = UET_FI_CONTAINER(fabric_fid, struct uet_fi_fabric, fabric_fid);
	/* The current engine owns one PDS instance and finalizes it with the
	 * domain. Keep that engine constraint explicit at the provider boundary.
	 */
	if (!atomic_compare_exchange_strong_explicit(
		    &fabric->domain_open, &expected, true, memory_order_acq_rel,
		    memory_order_acquire))
		return -FI_EBUSY;
	atomic_fetch_add_explicit(&fabric->refs, 1, memory_order_release);

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		rc = -FI_ENOMEM;
		goto err_ref;
	}
	domain->fabric = fabric;
	atomic_init(&domain->refs, 0);
	if (pthread_mutex_init(&domain->control_lock, NULL)) {
		rc = -FI_ENOMEM;
		goto err_free;
	}

	domain->domain_fid.fid.fclass = FI_CLASS_DOMAIN;
	domain->domain_fid.fid.context = context;
	domain->domain_fid.fid.ops = &uet_fi_domain_fid_ops;
	domain->domain_fid.ops = &uet_fi_domain_ops;
	domain->domain_fid.mr = &uet_fi_mr_ops;

	if (info->src_addr && info->src_addrlen == sizeof(struct uet_addr))
		family = info->src_addr;
	rc = fabric->engine.getinfo(fabric->uet, family, NULL,
				    &domain->engine_info);
	if (rc)
		goto err_mutex;
	if (fabric->engine.configure_info) {
		rc = fabric->engine.configure_info(fabric->uet,
					   domain->engine_info);
		if (rc)
			goto err_info;
	}
	uet_fi_merge_engine_info(domain->engine_info, info);

	rc = fabric->engine.domain(fabric->uet, fabric_fid,
				   domain->engine_info, &domain->domain_fid,
				   context, NULL, NULL, &domain->uet_domain);
	if (rc)
		goto err_info;

	*domain_out = &domain->domain_fid;
	return FI_SUCCESS;

err_info:
	fi_freeinfo(domain->engine_info);
err_mutex:
	pthread_mutex_destroy(&domain->control_lock);
err_free:
	free(domain);
err_ref:
	atomic_fetch_sub_explicit(&fabric->refs, 1, memory_order_release);
	atomic_store_explicit(&fabric->domain_open, false, memory_order_release);
	return rc;
}
