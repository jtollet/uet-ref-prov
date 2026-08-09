// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include <rdma/fi_errno.h>

#include "uet_fi.h"
#include "uet_fi_noop.h"

static int uet_fi_av_grow(struct uet_fi_av *av, size_t needed)
{
	struct uet_fi_av_entry **entries;
	size_t count;

	if (needed <= av->count)
		return FI_SUCCESS;
	count = av->count ? av->count : UET_FI_DEFAULT_AV;
	while (count < needed) {
		if (count > SIZE_MAX / 2)
			return -FI_ENOMEM;
		count *= 2;
	}
	entries = realloc(av->entries, count * sizeof(*entries));
	if (!entries)
		return -FI_ENOMEM;
	memset(entries + av->count, 0,
	       (count - av->count) * sizeof(*entries));
	av->entries = entries;
	av->count = count;
	return FI_SUCCESS;
}

static int uet_fi_av_close(struct fid *fid)
{
	struct uet_fi_av *av;
	struct uet_fi_engine *engine;
	size_t i;
	int rc;

	av = UET_FI_CONTAINER(fid, struct uet_fi_av, av_fid.fid);
	if (atomic_load_explicit(&av->refs, memory_order_acquire))
		return -FI_EBUSY;
	engine = &av->domain->fabric->engine;

	pthread_rwlock_wrlock(&av->lock);
	pthread_mutex_lock(&av->domain->control_lock);
	for (i = 0; i < av->count; i++) {
		if (!av->entries[i])
			continue;
		rc = engine->av_remove(av->entries[i]->uet_addr);
		if (rc) {
			pthread_mutex_unlock(&av->domain->control_lock);
			pthread_rwlock_unlock(&av->lock);
			return rc;
		}
		free(av->entries[i]);
	}
	pthread_mutex_unlock(&av->domain->control_lock);
	pthread_rwlock_unlock(&av->lock);

	pthread_rwlock_destroy(&av->lock);
	free(av->entries);
	atomic_fetch_sub_explicit(&av->domain->refs, 1, memory_order_release);
	free(av);
	return FI_SUCCESS;
}

static int uet_fi_av_insert(struct fid_av *av_fid, const void *addr,
			    size_t count, fi_addr_t *fi_addr, uint64_t flags,
			    void *context)
{
	struct uet_fi_av *av;
	const struct uet_addr *uet_addr = addr;
	struct uet_fi_av_entry *entry;
	size_t i, slot;
	int inserted = 0;
	int rc = FI_SUCCESS;

	(void)context;
	if (!addr || !count || !fi_addr || flags)
		return -FI_EINVAL;
	av = UET_FI_CONTAINER(av_fid, struct uet_fi_av, av_fid);

	pthread_rwlock_wrlock(&av->lock);
	pthread_mutex_lock(&av->domain->control_lock);
	for (i = 0; i < count; i++) {
		for (slot = 0; slot < av->count && av->entries[slot]; slot++)
			;
		if (slot == av->count) {
			rc = uet_fi_av_grow(av, av->count + 1);
			if (rc)
				break;
		}

		entry = calloc(1, sizeof(*entry));
		if (!entry) {
			rc = -FI_ENOMEM;
			break;
		}
		entry->addr = uet_addr[i];
		rc = av->domain->fabric->engine.av_insert(av->domain->uet_domain,
							    &entry->addr,
							    &entry->uet_addr);
		if (rc) {
			free(entry);
			break;
		}
		entry->active = true;
		av->entries[slot] = entry;
		fi_addr[i] = slot;
		inserted++;
	}
	pthread_mutex_unlock(&av->domain->control_lock);
	pthread_rwlock_unlock(&av->lock);

	return inserted ? inserted : rc;
}

static int uet_fi_av_remove(struct fid_av *av_fid, fi_addr_t *fi_addr,
			    size_t count, uint64_t flags)
{
	struct uet_fi_av *av;
	struct uet_fi_av_entry *entry;
	size_t i;
	int rc;

	if (!fi_addr || flags)
		return -FI_EINVAL;
	av = UET_FI_CONTAINER(av_fid, struct uet_fi_av, av_fid);

	pthread_rwlock_wrlock(&av->lock);
	pthread_mutex_lock(&av->domain->control_lock);
	for (i = 0; i < count; i++) {
		if (fi_addr[i] >= av->count || !av->entries[fi_addr[i]]) {
			rc = -FI_EINVAL;
			goto out;
		}
		entry = av->entries[fi_addr[i]];
		rc = av->domain->fabric->engine.av_remove(entry->uet_addr);
		if (rc)
			goto out;
		av->entries[fi_addr[i]] = NULL;
		free(entry);
	}
	rc = FI_SUCCESS;
out:
	pthread_mutex_unlock(&av->domain->control_lock);
	pthread_rwlock_unlock(&av->lock);
	return rc;
}

static int uet_fi_av_lookup(struct fid_av *av_fid, fi_addr_t fi_addr,
			    void *addr, size_t *addrlen)
{
	struct uet_fi_av *av;
	int rc = FI_SUCCESS;

	if (!addrlen)
		return -FI_EINVAL;
	av = UET_FI_CONTAINER(av_fid, struct uet_fi_av, av_fid);
	pthread_rwlock_rdlock(&av->lock);
	if (fi_addr >= av->count || !av->entries[fi_addr]) {
		rc = -FI_EINVAL;
		goto out;
	}
	if (!addr || *addrlen < sizeof(struct uet_addr)) {
		*addrlen = sizeof(struct uet_addr);
		rc = -FI_ETOOSMALL;
		goto out;
	}
	memcpy(addr, &av->entries[fi_addr]->addr, sizeof(struct uet_addr));
	*addrlen = sizeof(struct uet_addr);
out:
	pthread_rwlock_unlock(&av->lock);
	return rc;
}

static const char *uet_fi_av_straddr(struct fid_av *av_fid, const void *addr,
				     char *buf, size_t *len)
{
	const struct uet_addr *uet_addr = addr;
	char text[INET6_ADDRSTRLEN];
	struct in_addr in4;
	const void *src;
	size_t needed;

	(void)av_fid;
	if (!addr || !len)
		return NULL;
	if (uet_addr_is_ipv6(uet_addr)) {
		src = uet_addr->fa.v6;
		if (!inet_ntop(AF_INET6, src, text, sizeof(text)))
			return NULL;
	} else {
		in4.s_addr = htonl(uet_addr->fa.v4);
		if (!inet_ntop(AF_INET, &in4, text, sizeof(text)))
			return NULL;
	}
	needed = strlen(text) + 1;
	if (!buf || *len < needed) {
		*len = needed;
		return NULL;
	}
	memcpy(buf, text, needed);
	*len = needed;
	return buf;
}

static struct fi_ops uet_fi_av_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = uet_fi_av_close,
	.bind = uet_fi_no_bind,
	.control = uet_fi_no_control,
	.ops_open = uet_fi_no_ops_open,
	.tostr = uet_fi_no_tostr,
	.ops_set = uet_fi_no_ops_set,
};

static struct fi_ops_av uet_fi_av_ops = {
	.size = sizeof(struct fi_ops_av),
	.insert = uet_fi_av_insert,
	.insertsvc = uet_fi_no_av_insertsvc,
	.insertsym = uet_fi_no_av_insertsym,
	.remove = uet_fi_av_remove,
	.lookup = uet_fi_av_lookup,
	.straddr = uet_fi_av_straddr,
	.av_set = NULL,
};

int uet_fi_av_open(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		   struct fid_av **av_out, void *context)
{
	struct uet_fi_domain *domain;
	struct uet_fi_av *av;
	size_t count;

	if (!domain_fid || !av_out)
		return -FI_EINVAL;
	if (attr && attr->type != FI_AV_UNSPEC && attr->type != FI_AV_TABLE)
		return -FI_ENOSYS;
	if (attr && (attr->flags || attr->name || attr->rx_ctx_bits))
		return -FI_ENOSYS;

	domain = UET_FI_CONTAINER(domain_fid, struct uet_fi_domain, domain_fid);
	av = calloc(1, sizeof(*av));
	if (!av)
		return -FI_ENOMEM;
	count = attr && attr->count ? attr->count : UET_FI_DEFAULT_AV;
	av->entries = calloc(count, sizeof(*av->entries));
	if (!av->entries) {
		free(av);
		return -FI_ENOMEM;
	}
	if (pthread_rwlock_init(&av->lock, NULL)) {
		free(av->entries);
		free(av);
		return -FI_ENOMEM;
	}

	av->domain = domain;
	av->count = count;
	av->av_fid.fid.fclass = FI_CLASS_AV;
	av->av_fid.fid.context = context;
	av->av_fid.fid.ops = &uet_fi_av_fid_ops;
	av->av_fid.ops = &uet_fi_av_ops;
	atomic_init(&av->refs, 0);
	atomic_fetch_add_explicit(&domain->refs, 1, memory_order_release);
	*av_out = &av->av_fid;
	return FI_SUCCESS;
}

/* Caller holds av->lock for reading. */
struct uet_fi_av_entry *uet_fi_av_get(struct uet_fi_av *av,
				      fi_addr_t fi_addr)
{
	if (!av || fi_addr == FI_ADDR_UNSPEC || fi_addr >= av->count)
		return NULL;
	return av->entries[fi_addr];
}

fi_addr_t uet_fi_av_find_src(struct uet_fi_av *av, uint32_t initiator_id)
{
	fi_addr_t found = FI_ADDR_NOTAVAIL;
	size_t i;

	if (!av)
		return found;
	pthread_rwlock_rdlock(&av->lock);
	for (i = 0; i < av->count; i++) {
		if (av->entries[i] &&
		    av->entries[i]->addr.initiator_id == initiator_id) {
			found = i;
			break;
		}
	}
	pthread_rwlock_unlock(&av->lock);
	return found;
}

static void uet_fi_mr_unlink(struct uet_fi_mr *mr)
{
	struct uet_fi_mr **link;

	if (!mr->ep)
		return;
	for (link = &mr->ep->mrs; *link; link = &(*link)->ep_next) {
		if (*link == mr) {
			*link = mr->ep_next;
			break;
		}
	}
	mr->ep = NULL;
	mr->ep_next = NULL;
}

static int uet_fi_mr_close(struct fid *fid)
{
	struct uet_fi_mr *mr;
	struct uet_fi_engine *engine;
	int rc;

	mr = UET_FI_CONTAINER(fid, struct uet_fi_mr, mr_fid.fid);
	engine = &mr->domain->fabric->engine;
	pthread_mutex_lock(&mr->domain->control_lock);
	if (mr->enabled) {
		rc = engine->mr_disable(mr->uet_mr);
		if (rc)
			goto out;
		mr->enabled = false;
	}
	rc = engine->mr_close(mr->uet_mr);
	if (rc)
		goto out;
	uet_fi_mr_unlink(mr);
	pthread_mutex_unlock(&mr->domain->control_lock);
	atomic_fetch_sub_explicit(&mr->domain->refs, 1, memory_order_release);
	free(mr);
	return FI_SUCCESS;
out:
	pthread_mutex_unlock(&mr->domain->control_lock);
	return rc;
}

static int uet_fi_mr_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct uet_fi_mr *mr;
	struct uet_fi_ep *ep;
	int rc;

	if (!bfid || bfid->fclass != FI_CLASS_EP)
		return -FI_EINVAL;
	mr = UET_FI_CONTAINER(fid, struct uet_fi_mr, mr_fid.fid);
	ep = UET_FI_CONTAINER(bfid, struct uet_fi_ep, ep_fid.fid);
	if (ep->domain != mr->domain || mr->ep)
		return -FI_EINVAL;

	pthread_mutex_lock(&mr->domain->control_lock);
	rc = mr->domain->fabric->engine.ep_bind_mr(ep->uet_ep, mr->uet_mr,
						     flags);
	if (!rc) {
		mr->ep = ep;
		mr->ep_next = ep->mrs;
		ep->mrs = mr;
	}
	pthread_mutex_unlock(&mr->domain->control_lock);
	return rc;
}

static int uet_fi_mr_control(struct fid *fid, int command, void *arg)
{
	struct uet_fi_mr *mr;
	int rc;

	(void)arg;
	if (command != FI_ENABLE)
		return -FI_ENOSYS;
	mr = UET_FI_CONTAINER(fid, struct uet_fi_mr, mr_fid.fid);
	if (!mr->ep || mr->enabled)
		return -FI_EINVAL;

	pthread_mutex_lock(&mr->domain->control_lock);
	rc = mr->domain->fabric->engine.mr_enable(mr->uet_mr);
	if (!rc)
		mr->enabled = true;
	pthread_mutex_unlock(&mr->domain->control_lock);
	return rc;
}

static struct fi_ops uet_fi_mr_fid_ops = {
	.size = sizeof(struct fi_ops),
	.close = uet_fi_mr_close,
	.bind = uet_fi_mr_bind,
	.control = uet_fi_mr_control,
	.ops_open = uet_fi_no_ops_open,
	.tostr = uet_fi_no_tostr,
	.ops_set = uet_fi_no_ops_set,
};

static int uet_fi_mr_finish(struct uet_fi_domain *domain,
			    uet_mr_handle_t uet_mr, void *context,
			    struct fid_mr **mr_out)
{
	struct uet_fi_mr *mr;

	mr = calloc(1, sizeof(*mr));
	if (!mr) {
		domain->fabric->engine.mr_close(uet_mr);
		return -FI_ENOMEM;
	}
	mr->domain = domain;
	mr->uet_mr = uet_mr;
	mr->mr_fid.fid.fclass = FI_CLASS_MR;
	mr->mr_fid.fid.context = context;
	mr->mr_fid.fid.ops = &uet_fi_mr_fid_ops;
	mr->mr_fid.mem_desc = uet_mr;
	mr->mr_fid.key = domain->fabric->engine.mr_key(uet_mr);
	atomic_fetch_add_explicit(&domain->refs, 1, memory_order_release);
	*mr_out = &mr->mr_fid;
	return FI_SUCCESS;
}

int uet_fi_mr_reg(struct fid *fid, const void *buf, size_t len,
		  uint64_t access, uint64_t offset, uint64_t requested_key,
		  uint64_t flags, struct fid_mr **mr_out, void *context)
{
	struct uet_fi_domain *domain;
	uet_mr_handle_t uet_mr;
	int rc;

	if (!fid || fid->fclass != FI_CLASS_DOMAIN || !buf || !len || !mr_out ||
	    offset || flags)
		return -FI_EINVAL;
	domain = UET_FI_CONTAINER(fid, struct uet_fi_domain, domain_fid.fid);
	pthread_mutex_lock(&domain->control_lock);
	rc = domain->fabric->engine.mr_reg(domain->uet_domain, buf, len, access,
					   requested_key, UET_FLAGS_NONE,
					   context, &uet_mr);
	if (!rc)
		rc = uet_fi_mr_finish(domain, uet_mr, context, mr_out);
	pthread_mutex_unlock(&domain->control_lock);
	return rc;
}

int uet_fi_mr_regv(struct fid *fid, const struct iovec *iov, size_t count,
		   uint64_t access, uint64_t offset, uint64_t requested_key,
		   uint64_t flags, struct fid_mr **mr_out, void *context)
{
	struct uet_fi_domain *domain;
	uet_mr_handle_t uet_mr;
	int rc;

	if (!fid || fid->fclass != FI_CLASS_DOMAIN || !iov || !count || !mr_out ||
	    count > UET_FI_IOV_LIMIT || offset || flags)
		return -FI_EINVAL;
	domain = UET_FI_CONTAINER(fid, struct uet_fi_domain, domain_fid.fid);
	pthread_mutex_lock(&domain->control_lock);
	rc = domain->fabric->engine.mr_regv(domain->uet_domain, iov, count,
					    access, requested_key, UET_FLAGS_NONE,
					    context, &uet_mr);
	if (!rc)
		rc = uet_fi_mr_finish(domain, uet_mr, context, mr_out);
	pthread_mutex_unlock(&domain->control_lock);
	return rc;
}

int uet_fi_mr_regattr(struct fid *fid, const struct fi_mr_attr *attr,
		      uint64_t flags, struct fid_mr **mr_out)
{
	if (!attr || attr->iface != FI_HMEM_SYSTEM || attr->auth_key_size)
		return -FI_ENOSYS;
	return uet_fi_mr_regv(fid, attr->mr_iov, attr->iov_count, attr->access,
			      attr->offset, attr->requested_key, flags, mr_out,
			      attr->context);
}
