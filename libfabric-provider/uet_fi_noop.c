// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#include <rdma/fi_errno.h>

#include "uet_fi_noop.h"

#define UET_FI_UNUSED(x) ((void)(x))

int uet_fi_no_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	UET_FI_UNUSED(fid); UET_FI_UNUSED(bfid); UET_FI_UNUSED(flags);
	return -FI_ENOSYS;
}

int uet_fi_no_control(struct fid *fid, int command, void *arg)
{
	UET_FI_UNUSED(fid); UET_FI_UNUSED(command); UET_FI_UNUSED(arg);
	return -FI_ENOSYS;
}

int uet_fi_no_ops_open(struct fid *fid, const char *name, uint64_t flags,
		       void **ops, void *context)
{
	UET_FI_UNUSED(fid); UET_FI_UNUSED(name); UET_FI_UNUSED(flags);
	UET_FI_UNUSED(ops); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_tostr(const struct fid *fid, char *buf, size_t len)
{
	UET_FI_UNUSED(fid); UET_FI_UNUSED(buf); UET_FI_UNUSED(len);
	return -FI_ENOSYS;
}

int uet_fi_no_ops_set(struct fid *fid, const char *name, uint64_t flags,
		      void *ops, void *context)
{
	UET_FI_UNUSED(fid); UET_FI_UNUSED(name); UET_FI_UNUSED(flags);
	UET_FI_UNUSED(ops); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_passive_ep(struct fid_fabric *fabric, struct fi_info *info,
			 struct fid_pep **pep, void *context)
{
	UET_FI_UNUSED(fabric); UET_FI_UNUSED(info); UET_FI_UNUSED(pep);
	UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr,
		      struct fid_eq **eq, void *context)
{
	UET_FI_UNUSED(fabric); UET_FI_UNUSED(attr); UET_FI_UNUSED(eq);
	UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_wait_open(struct fid_fabric *fabric, struct fi_wait_attr *attr,
			struct fid_wait **waitset)
{
	UET_FI_UNUSED(fabric); UET_FI_UNUSED(attr); UET_FI_UNUSED(waitset);
	return -FI_ENOSYS;
}

int uet_fi_no_trywait(struct fid_fabric *fabric, struct fid **fids, int count)
{
	UET_FI_UNUSED(fabric); UET_FI_UNUSED(fids); UET_FI_UNUSED(count);
	return -FI_ENOSYS;
}

int uet_fi_no_domain2(struct fid_fabric *fabric, struct fi_info *info,
		      struct fid_domain **domain, uint64_t flags, void *context)
{
	UET_FI_UNUSED(fabric); UET_FI_UNUSED(info); UET_FI_UNUSED(domain);
	UET_FI_UNUSED(flags); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_scalable_ep(struct fid_domain *domain, struct fi_info *info,
			  struct fid_ep **sep, void *context)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(info); UET_FI_UNUSED(sep);
	UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_cntr_open(struct fid_domain *domain, struct fi_cntr_attr *attr,
			struct fid_cntr **cntr, void *context)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(attr); UET_FI_UNUSED(cntr);
	UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_poll_open(struct fid_domain *domain, struct fi_poll_attr *attr,
			struct fid_poll **pollset)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(attr); UET_FI_UNUSED(pollset);
	return -FI_ENOSYS;
}

int uet_fi_no_stx_context(struct fid_domain *domain, struct fi_tx_attr *attr,
			  struct fid_stx **stx, void *context)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(attr); UET_FI_UNUSED(stx);
	UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_srx_context(struct fid_domain *domain, struct fi_rx_attr *attr,
			  struct fid_ep **rx_ep, void *context)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(attr); UET_FI_UNUSED(rx_ep);
	UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_query_atomic(struct fid_domain *domain,
			   enum fi_datatype datatype, enum fi_op op,
			   struct fi_atomic_attr *attr, uint64_t flags)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(datatype); UET_FI_UNUSED(op);
	UET_FI_UNUSED(attr); UET_FI_UNUSED(flags);
	return -FI_ENOSYS;
}

int uet_fi_no_query_collective(struct fid_domain *domain,
			       enum fi_collective_op coll,
			       struct fi_collective_attr *attr, uint64_t flags)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(coll); UET_FI_UNUSED(attr);
	UET_FI_UNUSED(flags);
	return -FI_ENOSYS;
}

int uet_fi_no_endpoint2(struct fid_domain *domain, struct fi_info *info,
			struct fid_ep **ep, uint64_t flags, void *context)
{
	UET_FI_UNUSED(domain); UET_FI_UNUSED(info); UET_FI_UNUSED(ep);
	UET_FI_UNUSED(flags); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_getopt(fid_t fid, int level, int optname, void *optval,
		     size_t *optlen)
{
	UET_FI_UNUSED(fid); UET_FI_UNUSED(level); UET_FI_UNUSED(optname);
	UET_FI_UNUSED(optval); UET_FI_UNUSED(optlen);
	return -FI_ENOPROTOOPT;
}

int uet_fi_no_tx_ctx(struct fid_ep *sep, int index, struct fi_tx_attr *attr,
		     struct fid_ep **tx_ep, void *context)
{
	UET_FI_UNUSED(sep); UET_FI_UNUSED(index); UET_FI_UNUSED(attr);
	UET_FI_UNUSED(tx_ep); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_rx_ctx(struct fid_ep *sep, int index, struct fi_rx_attr *attr,
		     struct fid_ep **rx_ep, void *context)
{
	UET_FI_UNUSED(sep); UET_FI_UNUSED(index); UET_FI_UNUSED(attr);
	UET_FI_UNUSED(rx_ep); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

ssize_t uet_fi_no_rx_size_left(struct fid_ep *ep)
{
	UET_FI_UNUSED(ep); return -FI_ENOSYS;
}

ssize_t uet_fi_no_tx_size_left(struct fid_ep *ep)
{
	UET_FI_UNUSED(ep); return -FI_ENOSYS;
}

int uet_fi_no_setname(fid_t fid, void *addr, size_t addrlen)
{
	UET_FI_UNUSED(fid); UET_FI_UNUSED(addr); UET_FI_UNUSED(addrlen);
	return -FI_ENOSYS;
}

int uet_fi_no_getpeer(struct fid_ep *ep, void *addr, size_t *addrlen)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(addr); UET_FI_UNUSED(addrlen);
	return -FI_ENOSYS;
}

int uet_fi_no_connect(struct fid_ep *ep, const void *addr, const void *param,
		      size_t paramlen)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(addr); UET_FI_UNUSED(param);
	UET_FI_UNUSED(paramlen);
	return -FI_ENOSYS;
}

int uet_fi_no_listen(struct fid_pep *pep)
{
	UET_FI_UNUSED(pep); return -FI_ENOSYS;
}

int uet_fi_no_accept(struct fid_ep *ep, const void *param, size_t paramlen)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(param); UET_FI_UNUSED(paramlen);
	return -FI_ENOSYS;
}

int uet_fi_no_reject(struct fid_pep *pep, fid_t handle, const void *param,
		     size_t paramlen)
{
	UET_FI_UNUSED(pep); UET_FI_UNUSED(handle); UET_FI_UNUSED(param);
	UET_FI_UNUSED(paramlen);
	return -FI_ENOSYS;
}

int uet_fi_no_shutdown(struct fid_ep *ep, uint64_t flags)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(flags); return -FI_ENOSYS;
}

int uet_fi_no_join(struct fid_ep *ep, const void *addr, uint64_t flags,
		   struct fid_mc **mc, void *context)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(addr); UET_FI_UNUSED(flags);
	UET_FI_UNUSED(mc); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

ssize_t uet_fi_no_msg_inject(struct fid_ep *ep, const void *buf, size_t len,
			     fi_addr_t dest_addr)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(buf); UET_FI_UNUSED(len);
	UET_FI_UNUSED(dest_addr);
	return -FI_ENOSYS;
}

ssize_t uet_fi_no_msg_senddata(struct fid_ep *ep, const void *buf, size_t len,
			       void *desc, uint64_t data, fi_addr_t dest_addr,
			       void *context)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(buf); UET_FI_UNUSED(len);
	UET_FI_UNUSED(desc); UET_FI_UNUSED(data); UET_FI_UNUSED(dest_addr);
	UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

ssize_t uet_fi_no_msg_injectdata(struct fid_ep *ep, const void *buf,
				 size_t len, uint64_t data, fi_addr_t dest_addr)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(buf); UET_FI_UNUSED(len);
	UET_FI_UNUSED(data); UET_FI_UNUSED(dest_addr);
	return -FI_ENOSYS;
}

ssize_t uet_fi_no_tagged_inject(struct fid_ep *ep, const void *buf, size_t len,
				fi_addr_t dest_addr, uint64_t tag)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(buf); UET_FI_UNUSED(len);
	UET_FI_UNUSED(dest_addr); UET_FI_UNUSED(tag);
	return -FI_ENOSYS;
}

ssize_t uet_fi_no_tagged_senddata(struct fid_ep *ep, const void *buf,
				  size_t len, void *desc, uint64_t data,
				  fi_addr_t dest_addr, uint64_t tag,
				  void *context)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(buf); UET_FI_UNUSED(len);
	UET_FI_UNUSED(desc); UET_FI_UNUSED(data); UET_FI_UNUSED(dest_addr);
	UET_FI_UNUSED(tag); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

ssize_t uet_fi_no_tagged_injectdata(struct fid_ep *ep, const void *buf,
				    size_t len, uint64_t data,
				    fi_addr_t dest_addr, uint64_t tag)
{
	UET_FI_UNUSED(ep); UET_FI_UNUSED(buf); UET_FI_UNUSED(len);
	UET_FI_UNUSED(data); UET_FI_UNUSED(dest_addr); UET_FI_UNUSED(tag);
	return -FI_ENOSYS;
}

int uet_fi_no_cq_signal(struct fid_cq *cq)
{
	UET_FI_UNUSED(cq); return -FI_ENOSYS;
}

int uet_fi_no_av_insertsvc(struct fid_av *av, const char *node,
			   const char *service, fi_addr_t *fi_addr,
			   uint64_t flags, void *context)
{
	UET_FI_UNUSED(av); UET_FI_UNUSED(node); UET_FI_UNUSED(service);
	UET_FI_UNUSED(fi_addr); UET_FI_UNUSED(flags); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}

int uet_fi_no_av_insertsym(struct fid_av *av, const char *node,
			   size_t nodecnt, const char *service, size_t svccnt,
			   fi_addr_t *fi_addr, uint64_t flags, void *context)
{
	UET_FI_UNUSED(av); UET_FI_UNUSED(node); UET_FI_UNUSED(nodecnt);
	UET_FI_UNUSED(service); UET_FI_UNUSED(svccnt); UET_FI_UNUSED(fi_addr);
	UET_FI_UNUSED(flags); UET_FI_UNUSED(context);
	return -FI_ENOSYS;
}
