// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

#include <rdma/fi_errno.h>

#include "uet_fi.h"
#include "uet_fi_noop.h"

static int uet_fi_ep_close(struct fid *fid)
{
	struct uet_fi_ep *ep;
	struct uet_fi_mr *mr, *next;
	int rc;

	ep = UET_FI_CONTAINER(fid, struct uet_fi_ep, ep_fid.fid);
	pthread_mutex_lock(&ep->domain->control_lock);
	rc = ep->domain->fabric->engine.ep_close(ep->uet_ep);
	if (rc) {
		pthread_mutex_unlock(&ep->domain->control_lock);
		return rc;
	}

	for (mr = ep->mrs; mr; mr = next) {
		next = mr->ep_next;
		mr->ep = NULL;
		mr->ep_next = NULL;
		mr->enabled = false;
	}
	if (ep->tx_cq) {
		atomic_fetch_sub_explicit(&ep->tx_cq->refs, 1,
					  memory_order_release);
		ep->tx_cq->tx_cq = NULL;
	}
	if (ep->rx_cq) {
		atomic_fetch_sub_explicit(&ep->rx_cq->refs, 1,
					  memory_order_release);
		ep->rx_cq->rx_cq = NULL;
	}
	if (ep->tx_cq && ep->tx_cq == ep->rx_cq)
		ep->tx_cq->ep = NULL;
	else {
		if (ep->tx_cq)
			ep->tx_cq->ep = NULL;
		if (ep->rx_cq)
			ep->rx_cq->ep = NULL;
	}
	if (ep->av)
		atomic_fetch_sub_explicit(&ep->av->refs, 1, memory_order_release);
	pthread_mutex_unlock(&ep->domain->control_lock);

	fi_freeinfo(ep->engine_info);
	atomic_fetch_sub_explicit(&ep->domain->refs, 1, memory_order_release);
	free(ep);
	return FI_SUCCESS;
}

static int uet_fi_ep_bind_av(struct uet_fi_ep *ep, struct uet_fi_av *av,
			     uint64_t flags)
{
	if (flags || av->domain != ep->domain || ep->av)
		return -FI_EINVAL;
	ep->av = av;
	atomic_fetch_add_explicit(&av->refs, 1, memory_order_release);
	return FI_SUCCESS;
}

static int uet_fi_ep_bind_one_cq(struct uet_fi_ep *ep, struct uet_fi_cq *cq,
				 uint64_t direction)
{
	uet_cq_handle_t *handle;
	struct uet_fi_cq **ep_cq;
	int rc;

	if (cq->domain != ep->domain || (cq->ep && cq->ep != ep))
		return -FI_EINVAL;
	if (direction == FI_SEND) {
		handle = &cq->tx_cq;
		ep_cq = &ep->tx_cq;
	} else {
		handle = &cq->rx_cq;
		ep_cq = &ep->rx_cq;
	}
	if (*handle || *ep_cq)
		return -FI_EINVAL;

	rc = ep->domain->fabric->engine.ep_bind_cq(ep->uet_ep, &cq->attr,
						     &cq->cq_fid, direction,
						     cq->cq_fid.fid.context,
						     handle);
	if (!rc) {
		*ep_cq = cq;
		cq->ep = ep;
		atomic_fetch_add_explicit(&cq->refs, 1, memory_order_release);
	}
	return rc;
}

static int uet_fi_ep_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct uet_fi_ep *ep;
	struct uet_fi_cq *cq;
	int rc = FI_SUCCESS;

	if (!bfid)
		return -FI_EINVAL;
	ep = UET_FI_CONTAINER(fid, struct uet_fi_ep, ep_fid.fid);
	if (ep->enabled)
		return -FI_EOPBADSTATE;

	pthread_mutex_lock(&ep->domain->control_lock);
	switch (bfid->fclass) {
	case FI_CLASS_AV:
		rc = uet_fi_ep_bind_av(
			ep, UET_FI_CONTAINER(bfid, struct uet_fi_av, av_fid.fid),
			flags);
		break;
	case FI_CLASS_CQ:
		if (flags & ~(FI_SEND | FI_RECV)) {
			rc = -FI_EINVAL;
			break;
		}
		if (!(flags & (FI_SEND | FI_RECV))) {
			rc = -FI_EINVAL;
			break;
		}
		cq = UET_FI_CONTAINER(bfid, struct uet_fi_cq, cq_fid.fid);
		if (flags & FI_SEND) {
			rc = uet_fi_ep_bind_one_cq(ep, cq, FI_SEND);
			if (rc)
				break;
		}
		if (flags & FI_RECV)
			rc = uet_fi_ep_bind_one_cq(ep, cq, FI_RECV);
		break;
	default:
		rc = -FI_ENOSYS;
		break;
	}
	pthread_mutex_unlock(&ep->domain->control_lock);
	return rc;
}

static int uet_fi_ep_control(struct fid *fid, int command, void *arg)
{
	struct uet_fi_ep *ep;
	int rc;

	ep = UET_FI_CONTAINER(fid, struct uet_fi_ep, ep_fid.fid);
	pthread_mutex_lock(&ep->domain->control_lock);
	if (command == FI_ENABLE) {
		if (ep->enabled)
			rc = -FI_EOPBADSTATE;
		else {
			rc = ep->domain->fabric->engine.ep_enable(ep->uet_ep);
			if (!rc)
				ep->enabled = true;
		}
	} else {
		rc = ep->domain->fabric->engine.ep_control(ep->uet_ep,
							 command, arg);
	}
	pthread_mutex_unlock(&ep->domain->control_lock);
	return rc;
}

static ssize_t uet_fi_ep_cancel(fid_t fid, void *context)
{
	struct uet_fi_ep *ep;

	ep = UET_FI_CONTAINER(fid, struct uet_fi_ep, ep_fid.fid);
	return ep->domain->fabric->engine.cancel(ep->uet_ep, context);
}

static int uet_fi_ep_setopt(fid_t fid, int level, int optname,
			    const void *optval, size_t optlen)
{
	struct uet_fi_ep *ep;
	int rc;

	ep = UET_FI_CONTAINER(fid, struct uet_fi_ep, ep_fid.fid);
	rc = ep->domain->fabric->engine.ep_setopt(ep->uet_ep, level, optname,
						   optval, optlen);
	return rc == -FI_ENOSYS ? -FI_ENOPROTOOPT : rc;
}

static int uet_fi_ep_getname(fid_t fid, void *addr, size_t *addrlen)
{
	struct uet_fi_ep *ep;
	struct uet_addr uet_addr;
	int rc;

	if (!addrlen)
		return -FI_EINVAL;
	if (!addr || *addrlen < sizeof(uet_addr)) {
		*addrlen = sizeof(uet_addr);
		return -FI_ETOOSMALL;
	}
	ep = UET_FI_CONTAINER(fid, struct uet_fi_ep, ep_fid.fid);
	rc = ep->domain->fabric->engine.getname(ep->uet_ep, &uet_addr);
	if (rc)
		return rc;
	memcpy(addr, &uet_addr, sizeof(uet_addr));
	*addrlen = sizeof(uet_addr);
	return FI_SUCCESS;
}

static struct fi_ops uet_fi_ep_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = uet_fi_ep_close,
	.bind = uet_fi_ep_bind,
	.control = uet_fi_ep_control,
	.ops_open = uet_fi_no_ops_open,
	.tostr = uet_fi_no_tostr,
	.ops_set = uet_fi_no_ops_set,
};

static struct fi_ops_ep uet_fi_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = uet_fi_ep_cancel,
	.getopt = uet_fi_no_getopt,
	.setopt = uet_fi_ep_setopt,
	.tx_ctx = uet_fi_no_tx_ctx,
	.rx_ctx = uet_fi_no_rx_ctx,
	.rx_size_left = uet_fi_no_rx_size_left,
	.tx_size_left = uet_fi_no_tx_size_left,
};

static struct fi_ops_cm uet_fi_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = uet_fi_no_setname,
	.getname = uet_fi_ep_getname,
	.getpeer = uet_fi_no_getpeer,
	.connect = uet_fi_no_connect,
	.listen = uet_fi_no_listen,
	.accept = uet_fi_no_accept,
	.reject = uet_fi_no_reject,
	.shutdown = uet_fi_no_shutdown,
	.join = uet_fi_no_join,
};

static int uet_fi_ep_check_data(struct uet_fi_ep *ep, size_t count)
{
	if (!ep->enabled)
		return -FI_EOPBADSTATE;
	if (!count || count > UET_FI_IOV_LIMIT)
		return -FI_EINVAL;
	return FI_SUCCESS;
}

static uet_mr_handle_t uet_fi_one_desc(void **desc, size_t count, int *rc)
{
	uet_mr_handle_t first;
	size_t i;

	*rc = FI_SUCCESS;
	if (!desc)
		return UET_NULL_HANDLE;
	first = desc[0];
	for (i = 1; i < count; i++) {
		if (desc[i] != first) {
			*rc = -FI_EINVAL;
			return UET_NULL_HANDLE;
		}
	}
	return first;
}

static ssize_t uet_fi_msg_send(struct fid_ep *ep_fid, const void *buf,
			       size_t len, void *desc, fi_addr_t dest_addr,
			       void *context)
{
	struct uet_fi_av_entry *entry;
	struct uet_fi_ep *ep;
	ssize_t rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, 1);
	if (rc)
		return rc;
	if (!ep->av)
		return -FI_EINVAL;
	pthread_rwlock_rdlock(&ep->av->lock);
	entry = uet_fi_av_get(ep->av, dest_addr);
	if (!entry)
		rc = -FI_EINVAL;
	else
		rc = ep->domain->fabric->engine.send(ep->uet_ep, UET_DEF_JOB_ID,
						      (void *)buf, len, desc,
						      entry->uet_addr, context);
	pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_msg_sendv(struct fid_ep *ep_fid,
				const struct iovec *iov, void **desc,
				size_t count, fi_addr_t dest_addr,
				void *context)
{
	struct uet_fi_av_entry *entry;
	struct uet_fi_ep *ep;
	uet_mr_handle_t mr;
	ssize_t rc;
	int desc_rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, count);
	if (rc || !iov)
		return rc ? rc : -FI_EINVAL;
	mr = uet_fi_one_desc(desc, count, &desc_rc);
	if (desc_rc)
		return desc_rc;
	if (!ep->av)
		return -FI_EINVAL;
	pthread_rwlock_rdlock(&ep->av->lock);
	entry = uet_fi_av_get(ep->av, dest_addr);
	if (!entry)
		rc = -FI_EINVAL;
	else
		rc = ep->domain->fabric->engine.sendv(ep->uet_ep, UET_DEF_JOB_ID,
						       iov, count, mr,
						       entry->uet_addr, context);
	pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_msg_sendmsg(struct fid_ep *ep,
				  const struct fi_msg *msg, uint64_t flags)
{
	if (!msg || (flags & ~(FI_COMPLETION | FI_TRANSMIT_COMPLETE)))
		return -FI_ENOSYS;
	return uet_fi_msg_sendv(ep, msg->msg_iov, msg->desc, msg->iov_count,
				msg->addr, msg->context);
}

static ssize_t uet_fi_msg_recv(struct fid_ep *ep_fid, void *buf, size_t len,
			       void *desc, fi_addr_t src_addr, void *context)
{
	struct uet_fi_av_entry *entry = NULL;
	struct uet_fi_ep *ep;
	ssize_t rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, 1);
	if (rc)
		return rc;
	if ((ep->engine_info->caps & FI_DIRECTED_RECV) &&
	    src_addr != FI_ADDR_UNSPEC) {
		if (!ep->av)
			return -FI_EINVAL;
		pthread_rwlock_rdlock(&ep->av->lock);
		entry = uet_fi_av_get(ep->av, src_addr);
		if (!entry) {
			pthread_rwlock_unlock(&ep->av->lock);
			return -FI_EINVAL;
		}
	}
	rc = ep->domain->fabric->engine.recv(ep->uet_ep, UET_JOB_ID_ANY, buf,
						    len, desc,
						    entry ? entry->uet_addr : NULL,
						    context);
	if (entry)
		pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_msg_recvv(struct fid_ep *ep_fid,
				const struct iovec *iov, void **desc,
				size_t count, fi_addr_t src_addr,
				void *context)
{
	struct uet_fi_av_entry *entry = NULL;
	struct uet_fi_ep *ep;
	uet_mr_handle_t mr;
	ssize_t rc;
	int desc_rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, count);
	if (rc || !iov)
		return rc ? rc : -FI_EINVAL;
	mr = uet_fi_one_desc(desc, count, &desc_rc);
	if (desc_rc)
		return desc_rc;
	if ((ep->engine_info->caps & FI_DIRECTED_RECV) &&
	    src_addr != FI_ADDR_UNSPEC) {
		if (!ep->av)
			return -FI_EINVAL;
		pthread_rwlock_rdlock(&ep->av->lock);
		entry = uet_fi_av_get(ep->av, src_addr);
		if (!entry) {
			pthread_rwlock_unlock(&ep->av->lock);
			return -FI_EINVAL;
		}
	}
	rc = ep->domain->fabric->engine.recvv(ep->uet_ep, UET_JOB_ID_ANY,
						     iov, count, mr,
						     entry ? entry->uet_addr : NULL,
						     context);
	if (entry)
		pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_msg_recvmsg(struct fid_ep *ep,
				  const struct fi_msg *msg, uint64_t flags)
{
	if (!msg || (flags & ~FI_COMPLETION))
		return -FI_ENOSYS;
	return uet_fi_msg_recvv(ep, msg->msg_iov, msg->desc, msg->iov_count,
				msg->addr, msg->context);
}

static struct fi_ops_msg uet_fi_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = uet_fi_msg_recv,
	.recvv = uet_fi_msg_recvv,
	.recvmsg = uet_fi_msg_recvmsg,
	.send = uet_fi_msg_send,
	.sendv = uet_fi_msg_sendv,
	.sendmsg = uet_fi_msg_sendmsg,
	.inject = uet_fi_no_msg_inject,
	.senddata = uet_fi_no_msg_senddata,
	.injectdata = uet_fi_no_msg_injectdata,
};

static ssize_t uet_fi_tagged_send(struct fid_ep *ep_fid, const void *buf,
				  size_t len, void *desc, fi_addr_t dest_addr,
				  uint64_t tag, void *context)
{
	struct uet_fi_av_entry *entry;
	struct uet_fi_ep *ep;
	ssize_t rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, 1);
	if (rc)
		return rc;
	if (!ep->av)
		return -FI_EINVAL;
	pthread_rwlock_rdlock(&ep->av->lock);
	entry = uet_fi_av_get(ep->av, dest_addr);
	if (!entry)
		rc = -FI_EINVAL;
	else
		rc = ep->domain->fabric->engine.tsend(
			ep->uet_ep, UET_DEF_JOB_ID, (void *)buf, len, desc,
			entry->uet_addr, tag, context);
	pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_tagged_sendv(struct fid_ep *ep_fid,
				   const struct iovec *iov, void **desc,
				   size_t count, fi_addr_t dest_addr,
				   uint64_t tag, void *context)
{
	struct uet_fi_av_entry *entry;
	struct uet_fi_ep *ep;
	uet_mr_handle_t mr;
	ssize_t rc;
	int desc_rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, count);
	if (rc || !iov)
		return rc ? rc : -FI_EINVAL;
	mr = uet_fi_one_desc(desc, count, &desc_rc);
	if (desc_rc)
		return desc_rc;
	if (!ep->av)
		return -FI_EINVAL;
	pthread_rwlock_rdlock(&ep->av->lock);
	entry = uet_fi_av_get(ep->av, dest_addr);
	if (!entry)
		rc = -FI_EINVAL;
	else
		rc = ep->domain->fabric->engine.tsendv(
			ep->uet_ep, UET_DEF_JOB_ID, iov, count, mr,
			entry->uet_addr, tag, context);
	pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_tagged_sendmsg(struct fid_ep *ep,
				     const struct fi_msg_tagged *msg,
				     uint64_t flags)
{
	if (!msg || (flags & ~(FI_COMPLETION | FI_TRANSMIT_COMPLETE)))
		return -FI_ENOSYS;
	return uet_fi_tagged_sendv(ep, msg->msg_iov, msg->desc, msg->iov_count,
				   msg->addr, msg->tag, msg->context);
}

static ssize_t uet_fi_tagged_recv(struct fid_ep *ep_fid, void *buf,
				  size_t len, void *desc, fi_addr_t src_addr,
				  uint64_t tag, uint64_t ignore, void *context)
{
	struct uet_fi_av_entry *entry = NULL;
	struct uet_fi_ep *ep;
	ssize_t rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, 1);
	if (rc)
		return rc;
	if ((ep->engine_info->caps & FI_DIRECTED_RECV) &&
	    src_addr != FI_ADDR_UNSPEC) {
		if (!ep->av)
			return -FI_EINVAL;
		pthread_rwlock_rdlock(&ep->av->lock);
		entry = uet_fi_av_get(ep->av, src_addr);
		if (!entry) {
			pthread_rwlock_unlock(&ep->av->lock);
			return -FI_EINVAL;
		}
	}
	rc = ep->domain->fabric->engine.trecv(
		ep->uet_ep, UET_JOB_ID_ANY, buf, len, desc,
		entry ? entry->uet_addr : NULL, tag, ignore, context);
	if (entry)
		pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_tagged_recvv(struct fid_ep *ep_fid,
				   const struct iovec *iov, void **desc,
				   size_t count, fi_addr_t src_addr,
				   uint64_t tag, uint64_t ignore,
				   void *context)
{
	struct uet_fi_av_entry *entry = NULL;
	struct uet_fi_ep *ep;
	uet_mr_handle_t mr;
	ssize_t rc;
	int desc_rc;

	ep = UET_FI_CONTAINER(ep_fid, struct uet_fi_ep, ep_fid);
	rc = uet_fi_ep_check_data(ep, count);
	if (rc || !iov)
		return rc ? rc : -FI_EINVAL;
	mr = uet_fi_one_desc(desc, count, &desc_rc);
	if (desc_rc)
		return desc_rc;
	if ((ep->engine_info->caps & FI_DIRECTED_RECV) &&
	    src_addr != FI_ADDR_UNSPEC) {
		if (!ep->av)
			return -FI_EINVAL;
		pthread_rwlock_rdlock(&ep->av->lock);
		entry = uet_fi_av_get(ep->av, src_addr);
		if (!entry) {
			pthread_rwlock_unlock(&ep->av->lock);
			return -FI_EINVAL;
		}
	}
	rc = ep->domain->fabric->engine.trecvv(
		ep->uet_ep, UET_JOB_ID_ANY, iov, count, mr,
		entry ? entry->uet_addr : NULL, tag, ignore, context);
	if (entry)
		pthread_rwlock_unlock(&ep->av->lock);
	return rc;
}

static ssize_t uet_fi_tagged_recvmsg(struct fid_ep *ep,
				     const struct fi_msg_tagged *msg,
				     uint64_t flags)
{
	if (!msg || (flags & ~FI_COMPLETION))
		return -FI_ENOSYS;
	return uet_fi_tagged_recvv(ep, msg->msg_iov, msg->desc, msg->iov_count,
				   msg->addr, msg->tag, msg->ignore,
				   msg->context);
}

static struct fi_ops_tagged uet_fi_tagged_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = uet_fi_tagged_recv,
	.recvv = uet_fi_tagged_recvv,
	.recvmsg = uet_fi_tagged_recvmsg,
	.send = uet_fi_tagged_send,
	.sendv = uet_fi_tagged_sendv,
	.sendmsg = uet_fi_tagged_sendmsg,
	.inject = uet_fi_no_tagged_inject,
	.senddata = uet_fi_no_tagged_senddata,
	.injectdata = uet_fi_no_tagged_injectdata,
};

int uet_fi_endpoint_open(struct fid_domain *domain_fid, struct fi_info *info,
			 struct fid_ep **ep_out, void *context)
{
	struct uet_fi_domain *domain;
	struct uet_fi_ep *ep;
	int rc;

	if (!domain_fid || !info || !ep_out || !info->ep_attr ||
	    info->ep_attr->type != FI_EP_RDM)
		return -FI_EINVAL;
	domain = UET_FI_CONTAINER(domain_fid, struct uet_fi_domain, domain_fid);
	ep = calloc(1, sizeof(*ep));
	if (!ep)
		return -FI_ENOMEM;
	ep->domain = domain;
	ep->engine_info = fi_dupinfo(domain->engine_info);
	if (!ep->engine_info) {
		free(ep);
		return -FI_ENOMEM;
	}
	if (info->tx_attr && info->tx_attr->size)
		ep->engine_info->tx_attr->size = info->tx_attr->size;
	if (info->rx_attr && info->rx_attr->size)
		ep->engine_info->rx_attr->size = info->rx_attr->size;
	if (info->tx_attr)
		ep->engine_info->tx_attr->tclass = info->tx_attr->tclass;

	ep->ep_fid.fid.fclass = FI_CLASS_EP;
	ep->ep_fid.fid.context = context;
	ep->ep_fid.fid.ops = &uet_fi_ep_fid_ops;
	ep->ep_fid.ops = &uet_fi_ep_ops;
	ep->ep_fid.cm = &uet_fi_cm_ops;
	ep->ep_fid.msg = &uet_fi_msg_ops;
	ep->ep_fid.tagged = &uet_fi_tagged_ops;

	pthread_mutex_lock(&domain->control_lock);
	rc = domain->fabric->engine.endpoint(domain->uet_domain,
					     ep->engine_info, &ep->ep_fid,
					     context, &ep->uet_ep);
	pthread_mutex_unlock(&domain->control_lock);
	if (rc) {
		fi_freeinfo(ep->engine_info);
		free(ep);
		return rc;
	}
	atomic_fetch_add_explicit(&domain->refs, 1, memory_order_release);
	*ep_out = &ep->ep_fid;
	return FI_SUCCESS;
}
