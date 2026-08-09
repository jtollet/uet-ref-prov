/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 */

#ifndef UET_FI_NOOP_H
#define UET_FI_NOOP_H

#include <rdma/fi_atomic.h>
#include <rdma/fi_collective.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_tagged.h>

int uet_fi_no_bind(struct fid *fid, struct fid *bfid, uint64_t flags);
int uet_fi_no_control(struct fid *fid, int command, void *arg);
int uet_fi_no_ops_open(struct fid *fid, const char *name, uint64_t flags,
		       void **ops, void *context);
int uet_fi_no_tostr(const struct fid *fid, char *buf, size_t len);
int uet_fi_no_ops_set(struct fid *fid, const char *name, uint64_t flags,
		      void *ops, void *context);

int uet_fi_no_passive_ep(struct fid_fabric *fabric, struct fi_info *info,
			 struct fid_pep **pep, void *context);
int uet_fi_no_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr,
		      struct fid_eq **eq, void *context);
int uet_fi_no_wait_open(struct fid_fabric *fabric, struct fi_wait_attr *attr,
			struct fid_wait **waitset);
int uet_fi_no_trywait(struct fid_fabric *fabric, struct fid **fids, int count);
int uet_fi_no_domain2(struct fid_fabric *fabric, struct fi_info *info,
		      struct fid_domain **domain, uint64_t flags, void *context);

int uet_fi_no_scalable_ep(struct fid_domain *domain, struct fi_info *info,
			  struct fid_ep **sep, void *context);
int uet_fi_no_cntr_open(struct fid_domain *domain, struct fi_cntr_attr *attr,
			struct fid_cntr **cntr, void *context);
int uet_fi_no_poll_open(struct fid_domain *domain, struct fi_poll_attr *attr,
			struct fid_poll **pollset);
int uet_fi_no_stx_context(struct fid_domain *domain, struct fi_tx_attr *attr,
			  struct fid_stx **stx, void *context);
int uet_fi_no_srx_context(struct fid_domain *domain, struct fi_rx_attr *attr,
			  struct fid_ep **rx_ep, void *context);
int uet_fi_no_query_atomic(struct fid_domain *domain,
			   enum fi_datatype datatype, enum fi_op op,
			   struct fi_atomic_attr *attr, uint64_t flags);
int uet_fi_no_query_collective(struct fid_domain *domain,
			       enum fi_collective_op coll,
			       struct fi_collective_attr *attr, uint64_t flags);
int uet_fi_no_endpoint2(struct fid_domain *domain, struct fi_info *info,
			struct fid_ep **ep, uint64_t flags, void *context);

int uet_fi_no_getopt(fid_t fid, int level, int optname, void *optval,
		     size_t *optlen);
int uet_fi_no_tx_ctx(struct fid_ep *sep, int index, struct fi_tx_attr *attr,
		     struct fid_ep **tx_ep, void *context);
int uet_fi_no_rx_ctx(struct fid_ep *sep, int index, struct fi_rx_attr *attr,
		     struct fid_ep **rx_ep, void *context);
ssize_t uet_fi_no_rx_size_left(struct fid_ep *ep);
ssize_t uet_fi_no_tx_size_left(struct fid_ep *ep);

int uet_fi_no_setname(fid_t fid, void *addr, size_t addrlen);
int uet_fi_no_getpeer(struct fid_ep *ep, void *addr, size_t *addrlen);
int uet_fi_no_connect(struct fid_ep *ep, const void *addr, const void *param,
		      size_t paramlen);
int uet_fi_no_listen(struct fid_pep *pep);
int uet_fi_no_accept(struct fid_ep *ep, const void *param, size_t paramlen);
int uet_fi_no_reject(struct fid_pep *pep, fid_t handle, const void *param,
		     size_t paramlen);
int uet_fi_no_shutdown(struct fid_ep *ep, uint64_t flags);
int uet_fi_no_join(struct fid_ep *ep, const void *addr, uint64_t flags,
		   struct fid_mc **mc, void *context);

ssize_t uet_fi_no_msg_inject(struct fid_ep *ep, const void *buf, size_t len,
			     fi_addr_t dest_addr);
ssize_t uet_fi_no_msg_senddata(struct fid_ep *ep, const void *buf, size_t len,
			       void *desc, uint64_t data, fi_addr_t dest_addr,
			       void *context);
ssize_t uet_fi_no_msg_injectdata(struct fid_ep *ep, const void *buf,
				 size_t len, uint64_t data,
				 fi_addr_t dest_addr);
ssize_t uet_fi_no_tagged_inject(struct fid_ep *ep, const void *buf, size_t len,
				fi_addr_t dest_addr, uint64_t tag);
ssize_t uet_fi_no_tagged_senddata(struct fid_ep *ep, const void *buf,
				  size_t len, void *desc, uint64_t data,
				  fi_addr_t dest_addr, uint64_t tag,
				  void *context);
ssize_t uet_fi_no_tagged_injectdata(struct fid_ep *ep, const void *buf,
				    size_t len, uint64_t data,
				    fi_addr_t dest_addr, uint64_t tag);
int uet_fi_no_cq_signal(struct fid_cq *cq);
int uet_fi_no_av_insertsvc(struct fid_av *av, const char *node,
			   const char *service, fi_addr_t *fi_addr,
			   uint64_t flags, void *context);
int uet_fi_no_av_insertsym(struct fid_av *av, const char *node,
			   size_t nodecnt, const char *service, size_t svccnt,
			   fi_addr_t *fi_addr, uint64_t flags, void *context);

#endif /* UET_FI_NOOP_H */
