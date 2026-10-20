// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * Jobs, job keys, and job address tables.
 *
 * Allocating a job and configuring its address table are privileged. An
 * unprivileged process reaches a job by importing a file descriptor somebody
 * handed it.
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>

#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_user_verbs.h>

#include "uet_ref.h"

/* ---------------------------------------------------------------- */
/* jobs                                                             */
/* ---------------------------------------------------------------- */

/* ib_core owns the object, its sharing between processes, its lifetime, and
 * its destroy ordering.
 */
int uet_ref_alloc_job(struct ib_job *ibjob,
		      struct ib_job_attr *attr,
		      struct uverbs_attr_bundle *attrs)
{
	struct uet_ref_job *job = to_uet_job(ibjob);
	struct uet_ref_dev *dev = container_of(ibjob->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[4];
	u64 result[2];
	int ret;

	param[0] = attr->job_id;
	param[1] = attr->max_addr_entries;
	param[2] = attr->port_num | ((u64)attr->sgid_index << 8);
	param[3] = 0;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_JOB_ALLOC, param,
				ARRAY_SIZE(param), NULL, 0, result);
	if (ret)
		return ret;

	job->handle = (u32)result[0];

	return 0;
}

void uet_ref_dealloc_job(struct ib_job *ibjob)
{
	struct uet_ref_dev *dev = container_of(ibjob->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[1] = { to_uet_job(ibjob)->handle };
	int ret;

	/* Nothing above can act on a failure: this runs from the job's last
	 * reference.
	 */
	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_JOB_FREE, param, 1, NULL,
				0, NULL);
	if (ret)
		dev_warn(&dev->pdev->dev, "job %u not freed: %d\n",
			 to_uet_job(ibjob)->handle, ret);
}

int uet_ref_query_job(struct ib_job *ibjob,
		      struct ib_job_attr *attr)
{
	struct uet_ref_dev *dev = container_of(ibjob->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_dev_admin_job_info info;
	u64 param[1] = { to_uet_job(ibjob)->handle };
	int ret;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_JOB_QUERY, param, 1,
				&info, sizeof(info), NULL);
	if (ret)
		return ret;

	attr->job_id = le32_to_cpu(info.job_id);
	attr->max_addr_entries = le32_to_cpu(info.max_addr_entries);
	attr->port_num = info.port_num;
	attr->sgid_index = info.sgid_index;

	return 0;
}

/* ---------------------------------------------------------------- */
/* job address table                                                */
/* ---------------------------------------------------------------- */

/* Translate the core's address into what the device stores. The MAC is
 * left for the device to resolve.
 */
static void uet_ref_addr_from_rdma(struct uet_dev_admin_addr *out,
				   const struct rdma_ah_attr_ex *in_ex)
{
	const struct rdma_ah_attr *in = &in_ex->ah_attr;
	const struct ib_global_route *grh;

	memset(out, 0, sizeof(*out));

	out->sl = rdma_ah_get_sl(in);
	out->port_num = rdma_ah_get_port_num(in);
	out->is_global = !!(rdma_ah_get_ah_flags(in) & IB_AH_GRH);
	out->remote_qpn = cpu_to_le32(in_ex->remote_qpn);

	/* RoCE keeps the destination MAC out of the grh */
	if (in->type == RDMA_AH_ATTR_TYPE_ROCE)
		memcpy(out->dmac, in->roce.dmac, sizeof(out->dmac));

	if (!out->is_global)
		return;

	grh = rdma_ah_read_grh(in);

	memcpy(out->dgid, grh->dgid.raw, sizeof(out->dgid));
	out->flow_label = cpu_to_le32(grh->flow_label);
	out->sgid_index = grh->sgid_index;
	out->hop_limit = grh->hop_limit;
	out->traffic_class = grh->traffic_class;
}

static void uet_ref_addr_to_rdma(struct rdma_ah_attr_ex *out_ex,
				 const struct uet_dev_admin_addr *in,
				 struct ib_device *ibdev)
{
	struct rdma_ah_attr *out = &out_ex->ah_attr;

	memset(out_ex, 0, sizeof(*out_ex));
	out_ex->remote_qpn = le32_to_cpu(in->remote_qpn);
	out->type = rdma_ah_find_type(ibdev, in->port_num);

	rdma_ah_set_sl(out, in->sl);
	rdma_ah_set_port_num(out, in->port_num);

	if (!in->is_global) {
		rdma_ah_set_ah_flags(out, 0);
		return;
	}

	rdma_ah_set_grh(out, NULL, le32_to_cpu(in->flow_label),
			in->sgid_index, in->hop_limit, in->traffic_class);
	rdma_ah_set_dgid_raw(out, (void *)in->dgid);
}

/* Core may run these concurrently on one job. The table lives in the device
 * and every command reaches it through the admin queue, whose lock
 * serializes them.
 */
int uet_ref_job_addr_insert(struct ib_job *ibjob,
			    u32 index,
			    struct rdma_ah_attr_ex *ah_attr)
{
	struct uet_ref_job *job = to_uet_job(ibjob);
	struct uet_ref_dev *dev = container_of(ibjob->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_dev_admin_addr addr;
	u64 param[3] = { job->handle, index, 0 };

	uet_ref_addr_from_rdma(&addr, ah_attr);

	return uet_ref_admin_cmd(dev, UET_DEV_ADMIN_ADDR_INSERT, param,
				 ARRAY_SIZE(param), &addr, sizeof(addr),
				 NULL);
}

int uet_ref_job_addr_remove(struct ib_job *ibjob,
			    u32 index)
{
	struct uet_ref_job *job = to_uet_job(ibjob);
	struct uet_ref_dev *dev = container_of(ibjob->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[3] = { job->handle, index, 0 };

	return uet_ref_admin_cmd(dev, UET_DEV_ADMIN_ADDR_REMOVE, param,
				 ARRAY_SIZE(param), NULL, 0, NULL);
}

int uet_ref_job_addr_query(struct ib_job *ibjob,
			   u32 index,
			   struct rdma_ah_attr_ex *ah_attr)
{
	struct uet_ref_job *job = to_uet_job(ibjob);
	struct uet_ref_dev *dev = container_of(ibjob->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_dev_admin_addr addr;
	u64 param[3] = { job->handle, index, 0 };
	int ret;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_ADDR_QUERY, param,
				ARRAY_SIZE(param), &addr, sizeof(addr), NULL);
	if (ret)
		return ret;

	uet_ref_addr_to_rdma(ah_attr, &addr, ibjob->device);

	return 0;
}

/* ---------------------------------------------------------------- */
/* job keys                                                         */
/* ---------------------------------------------------------------- */

int uet_ref_create_jkey(struct ib_jkey *ibjkey,
			struct uverbs_attr_bundle *attrs)
{
	struct uet_ref_jkey *jkey = to_uet_jkey(ibjkey);
	struct uet_ref_job *job = to_uet_job(ibjkey->job);
	struct uet_ref_dev *dev = container_of(ibjkey->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_ref_pd *pd = container_of(ibjkey->pd, struct uet_ref_pd,
					     ibpd);
	u64 param[3];
	u64 result[2];
	int ret;

	param[0] = job->handle;
	param[1] = pd->pdn;
	param[2] = 0;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_JKEY_CREATE, param,
				ARRAY_SIZE(param), NULL, 0, result);
	if (ret)
		return ret;

	jkey->handle = (u32)result[0];
	ibjkey->jkey = (u32)result[1];

	return 0;
}

int uet_ref_destroy_jkey(struct ib_jkey *ibjkey,
			 struct uverbs_attr_bundle *attrs)
{
	struct uet_ref_dev *dev = container_of(ibjkey->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[1] = { to_uet_jkey(ibjkey)->handle };

	/* Freeing a job releases the keys that named it, so by the time a
	 * job's teardown reaches here the device may already have dropped
	 * this key. A stale handle is answered with ENOENT, and the object
	 * goes either way.
	 */
	uet_ref_admin_cmd(dev, UET_DEV_ADMIN_JKEY_DESTROY, param, 1, NULL,
			  0, NULL);

	return 0;
}

