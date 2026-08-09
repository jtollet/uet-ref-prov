// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rdma/fi_errno.h>

#include "uet_fi.h"
#include "uet_fi_noop.h"

static size_t uet_fi_cq_entry_size(enum fi_cq_format format)
{
	switch (format) {
	case FI_CQ_FORMAT_CONTEXT:
	case FI_CQ_FORMAT_UNSPEC:
		return sizeof(struct fi_cq_entry);
	case FI_CQ_FORMAT_MSG:
		return sizeof(struct fi_cq_msg_entry);
	case FI_CQ_FORMAT_DATA:
		return sizeof(struct fi_cq_data_entry);
	case FI_CQ_FORMAT_TAGGED:
		return sizeof(struct fi_cq_tagged_entry);
	default:
		return 0;
	}
}

static int uet_fi_cq_close(struct fid *fid)
{
	struct uet_fi_cq *cq;

	cq = UET_FI_CONTAINER(fid, struct uet_fi_cq, cq_fid.fid);
	if (atomic_load_explicit(&cq->refs, memory_order_acquire))
		return -FI_EBUSY;
	pthread_mutex_destroy(&cq->lock);
	atomic_fetch_sub_explicit(&cq->domain->refs, 1, memory_order_release);
	free(cq);
	return FI_SUCCESS;
}

static ssize_t uet_fi_cq_read_handle(struct uet_fi_cq *cq,
				     uet_cq_handle_t handle, void *buf,
				     size_t count)
{
	ssize_t rc;

	if (!handle)
		return -FI_EAGAIN;
	rc = cq->domain->fabric->engine.cq_read(handle, buf, count);
	return rc ? rc : -FI_EAGAIN;
}

static ssize_t uet_fi_cq_read_internal(struct uet_fi_cq *cq, void *buf,
				       size_t count, bool *is_rx,
				       uet_cq_handle_t *used)
{
	uet_cq_handle_t first, second;
	bool first_rx;
	ssize_t rc;

	if (!count)
		return -FI_EINVAL;
	if (cq->tx_cq && cq->rx_cq) {
		first_rx = cq->read_rx_next;
		first = first_rx ? cq->rx_cq : cq->tx_cq;
		second = first_rx ? cq->tx_cq : cq->rx_cq;
		cq->read_rx_next = !cq->read_rx_next;
	} else {
		first = cq->rx_cq ? cq->rx_cq : cq->tx_cq;
		second = NULL;
		first_rx = !!cq->rx_cq;
	}

	rc = uet_fi_cq_read_handle(cq, first, buf, count);
	if (rc != -FI_EAGAIN) {
		if (is_rx)
			*is_rx = first_rx;
		if (used)
			*used = first;
		return rc;
	}
	if (!second)
		return rc;
	rc = uet_fi_cq_read_handle(cq, second, buf, count);
	if (rc != -FI_EAGAIN) {
		if (is_rx)
			*is_rx = !first_rx;
		if (used)
			*used = second;
	}
	return rc;
}

static ssize_t uet_fi_cq_read(struct fid_cq *cq_fid, void *buf, size_t count)
{
	struct uet_fi_cq *cq;
	ssize_t rc;

	if (!buf)
		return -FI_EINVAL;
	cq = UET_FI_CONTAINER(cq_fid, struct uet_fi_cq, cq_fid);
	pthread_mutex_lock(&cq->lock);
	rc = uet_fi_cq_read_internal(cq, buf, count, NULL, NULL);
	pthread_mutex_unlock(&cq->lock);
	return rc;
}

static ssize_t uet_fi_cq_readfrom(struct fid_cq *cq_fid, void *buf,
				  size_t count, fi_addr_t *src_addr)
{
	struct uet_fi_cq *cq;
	uet_cq_handle_t used;
	size_t entry_size;
	ssize_t done = 0;
	ssize_t rc;
	bool is_rx;

	if (!buf || !src_addr || !count)
		return -FI_EINVAL;
	cq = UET_FI_CONTAINER(cq_fid, struct uet_fi_cq, cq_fid);
	entry_size = uet_fi_cq_entry_size(cq->attr.format);
	pthread_mutex_lock(&cq->lock);
	while ((size_t)done < count) {
		rc = uet_fi_cq_read_internal(cq, (char *)buf + done * entry_size,
					     1, &is_rx, &used);
		if (rc < 0) {
			rc = done ? done : rc;
			goto out;
		}
		if (is_rx && cq->ep && cq->ep->av)
			src_addr[done] = uet_fi_av_find_src(
				cq->ep->av,
				cq->domain->fabric->engine.cq_read_src_id(used));
		else
			src_addr[done] = FI_ADDR_NOTAVAIL;
		done++;
	}
	rc = done;
out:
	pthread_mutex_unlock(&cq->lock);
	return rc;
}

static ssize_t uet_fi_cq_readerr(struct fid_cq *cq_fid,
				 struct fi_cq_err_entry *buf, uint64_t flags)
{
	struct uet_fi_cq *cq;
	ssize_t rc;

	if (!buf || flags)
		return -FI_EINVAL;
	cq = UET_FI_CONTAINER(cq_fid, struct uet_fi_cq, cq_fid);
	pthread_mutex_lock(&cq->lock);
	if (cq->tx_cq) {
		rc = cq->domain->fabric->engine.cq_readerr(cq->tx_cq, buf);
		if (rc != -FI_EAGAIN)
			goto out;
	}
	if (cq->rx_cq)
		rc = cq->domain->fabric->engine.cq_readerr(cq->rx_cq, buf);
	else
		rc = -FI_EAGAIN;
out:
	pthread_mutex_unlock(&cq->lock);
	return rc;
}

static uint64_t uet_fi_monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static ssize_t uet_fi_cq_sread_common(struct fid_cq *cq_fid, void *buf,
				      size_t count, fi_addr_t *src_addr,
				      const void *cond, int timeout)
{
	const size_t *threshold_ptr = cond;
	struct uet_fi_cq *cq;
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 50000 };
	uint64_t deadline = 0;
	size_t threshold = 1;
	ssize_t rc;

	cq = UET_FI_CONTAINER(cq_fid, struct uet_fi_cq, cq_fid);
	if (cq->attr.wait_cond == FI_CQ_COND_THRESHOLD && threshold_ptr)
		threshold = *threshold_ptr;
	if (!threshold || threshold > count)
		return -FI_EINVAL;
	if (timeout >= 0)
		deadline = uet_fi_monotonic_ms() + (uint64_t)timeout;

	for (;;) {
		rc = src_addr ? uet_fi_cq_readfrom(cq_fid, buf, count, src_addr) :
				uet_fi_cq_read(cq_fid, buf, count);
		if (rc >= (ssize_t)threshold || rc == -FI_EAVAIL)
			return rc;
		if (rc > 0)
			return rc;
		if (rc != -FI_EAGAIN)
			return rc;
		if (!timeout || (timeout > 0 &&
		    uet_fi_monotonic_ms() >= deadline))
			return -FI_EAGAIN;
		nanosleep(&pause, NULL);
	}
}

static ssize_t uet_fi_cq_sread(struct fid_cq *cq, void *buf, size_t count,
			       const void *cond, int timeout)
{
	return uet_fi_cq_sread_common(cq, buf, count, NULL, cond, timeout);
}

static ssize_t uet_fi_cq_sreadfrom(struct fid_cq *cq, void *buf, size_t count,
				   fi_addr_t *src_addr, const void *cond,
				   int timeout)
{
	return uet_fi_cq_sread_common(cq, buf, count, src_addr, cond, timeout);
}

static const char *uet_fi_cq_strerror(struct fid_cq *cq, int prov_errno,
				      const void *err_data, char *buf,
				      size_t len)
{
	const char *text = fi_strerror(prov_errno);
	size_t copy_len;

	(void)cq;
	(void)err_data;
	if (buf && len) {
		copy_len = strnlen(text, len - 1);
		memcpy(buf, text, copy_len);
		buf[copy_len] = '\0';
		return buf;
	}
	return text;
}

static struct fi_ops uet_fi_cq_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = uet_fi_cq_close,
	.bind = uet_fi_no_bind,
	.control = uet_fi_no_control,
	.ops_open = uet_fi_no_ops_open,
	.tostr = uet_fi_no_tostr,
	.ops_set = uet_fi_no_ops_set,
};

static struct fi_ops_cq uet_fi_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = uet_fi_cq_read,
	.readfrom = uet_fi_cq_readfrom,
	.readerr = uet_fi_cq_readerr,
	.sread = uet_fi_cq_sread,
	.sreadfrom = uet_fi_cq_sreadfrom,
	.signal = uet_fi_no_cq_signal,
	.strerror = uet_fi_cq_strerror,
};

int uet_fi_cq_open(struct fid_domain *domain_fid, struct fi_cq_attr *attr,
		   struct fid_cq **cq_out, void *context)
{
	struct uet_fi_domain *domain;
	struct uet_fi_cq *cq;

	if (!domain_fid || !attr || !cq_out ||
	    !uet_fi_cq_entry_size(attr->format))
		return -FI_EINVAL;
	if (attr->wait_obj != FI_WAIT_NONE && attr->wait_obj != FI_WAIT_UNSPEC)
		return -FI_ENOSYS;
	if (attr->flags || attr->wait_set)
		return -FI_ENOSYS;

	domain = UET_FI_CONTAINER(domain_fid, struct uet_fi_domain, domain_fid);
	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return -FI_ENOMEM;
	if (pthread_mutex_init(&cq->lock, NULL)) {
		free(cq);
		return -FI_ENOMEM;
	}
	cq->domain = domain;
	cq->attr = *attr;
	if (!cq->attr.size)
		cq->attr.size = UET_FI_DEFAULT_QUEUE;
	if (cq->attr.format == FI_CQ_FORMAT_UNSPEC)
		cq->attr.format = FI_CQ_FORMAT_CONTEXT;
	cq->cq_fid.fid.fclass = FI_CLASS_CQ;
	cq->cq_fid.fid.context = context;
	cq->cq_fid.fid.ops = &uet_fi_cq_fid_ops;
	cq->cq_fid.ops = &uet_fi_cq_ops;
	atomic_init(&cq->refs, 0);
	atomic_fetch_add_explicit(&domain->refs, 1, memory_order_release);
	*cq_out = &cq->cq_fid;
	return FI_SUCCESS;
}
