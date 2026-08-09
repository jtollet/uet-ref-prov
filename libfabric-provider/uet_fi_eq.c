// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

#include <rdma/fi_errno.h>

#include "uet_fi.h"
#include "uet_fi_noop.h"

static int uet_fi_eq_close(struct fid *fid)
{
	struct uet_fi_eq *eq;

	eq = UET_FI_CONTAINER(fid, struct uet_fi_eq, eq_fid.fid);
	atomic_fetch_sub_explicit(&eq->fabric->refs, 1, memory_order_release);
	free(eq);
	return FI_SUCCESS;
}

static ssize_t uet_fi_eq_read(struct fid_eq *eq, uint32_t *event, void *buf,
			      size_t len, uint64_t flags)
{
	(void)eq;
	(void)event;
	(void)buf;
	(void)len;
	if (flags & ~FI_PEEK)
		return -FI_EINVAL;
	return -FI_EAGAIN;
}

static ssize_t uet_fi_eq_readerr(struct fid_eq *eq,
				 struct fi_eq_err_entry *buf, uint64_t flags)
{
	(void)eq;
	(void)buf;
	return flags ? -FI_EINVAL : -FI_EAGAIN;
}

static ssize_t uet_fi_eq_write(struct fid_eq *eq, uint32_t event,
			       const void *buf, size_t len, uint64_t flags)
{
	(void)eq;
	(void)event;
	(void)buf;
	(void)len;
	(void)flags;
	return -FI_ENOSYS;
}

static ssize_t uet_fi_eq_sread(struct fid_eq *eq, uint32_t *event, void *buf,
			       size_t len, int timeout, uint64_t flags)
{
	(void)timeout;
	return uet_fi_eq_read(eq, event, buf, len, flags);
}

static const char *uet_fi_eq_strerror(struct fid_eq *eq, int prov_errno,
				     const void *err_data, char *buf,
				     size_t len)
{
	const char *text = fi_strerror(prov_errno);
	size_t copy_len;

	(void)eq;
	(void)err_data;
	if (buf && len) {
		copy_len = strnlen(text, len - 1);
		memcpy(buf, text, copy_len);
		buf[copy_len] = '\0';
		return buf;
	}
	return text;
}

static struct fi_ops uet_fi_eq_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = uet_fi_eq_close,
	.bind = uet_fi_no_bind,
	.control = uet_fi_no_control,
	.ops_open = uet_fi_no_ops_open,
	.tostr = uet_fi_no_tostr,
	.ops_set = uet_fi_no_ops_set,
};

static struct fi_ops_eq uet_fi_eq_ops = {
	.size = sizeof(struct fi_ops_eq),
	.read = uet_fi_eq_read,
	.readerr = uet_fi_eq_readerr,
	.write = uet_fi_eq_write,
	.sread = uet_fi_eq_sread,
	.strerror = uet_fi_eq_strerror,
};

int uet_fi_eq_open(struct fid_fabric *fabric_fid, struct fi_eq_attr *attr,
		   struct fid_eq **eq_out, void *context)
{
	struct uet_fi_fabric *fabric;
	struct uet_fi_eq *eq;

	if (!fabric_fid || !eq_out)
		return -FI_EINVAL;
	if (attr && (attr->flags || attr->wait_set ||
	    (attr->wait_obj != FI_WAIT_NONE &&
	     attr->wait_obj != FI_WAIT_UNSPEC)))
		return -FI_ENOSYS;

	fabric = UET_FI_CONTAINER(fabric_fid, struct uet_fi_fabric, fabric_fid);
	eq = calloc(1, sizeof(*eq));
	if (!eq)
		return -FI_ENOMEM;
	eq->fabric = fabric;
	eq->eq_fid.fid.fclass = FI_CLASS_EQ;
	eq->eq_fid.fid.context = context;
	eq->eq_fid.fid.ops = &uet_fi_eq_fid_ops;
	eq->eq_fid.ops = &uet_fi_eq_ops;
	atomic_fetch_add_explicit(&fabric->refs, 1, memory_order_release);
	*eq_out = &eq->eq_fid;
	return FI_SUCCESS;
}
