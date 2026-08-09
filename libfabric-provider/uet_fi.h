/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef UET_FI_H
#define UET_FI_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_tagged.h>

#include "uet_api.h"

#define UET_FI_PROV_NAME        "uet"
#define UET_FI_FABRIC_NAME      "uet"
#define UET_FI_DOMAIN_NAME      "uet"
#define UET_FI_DEFAULT_QUEUE    256
#define UET_FI_DEFAULT_AV       64
#define UET_FI_IOV_LIMIT        8
#define UET_FI_MAX_MSG_SIZE     ((size_t)UINT32_MAX - 1)

struct uet_fi_engine {
	void *dl_handle;
	const char *name;

	int (*initialize)(uet_handle_t *handle);
	int (*finalize)(uet_handle_t handle);
	int (*getinfo)(uet_handle_t handle, struct uet_addr *node,
		       const struct fi_info *hints, struct fi_info **info);
	int (*domain)(uet_handle_t handle, struct fid_fabric *fabric,
		      struct fi_info *info, struct fid_domain *domain,
		      void *context, uet_eq_callback_t eq_callback,
		      uet_eq_err_callback_t eq_err_callback,
		      uet_domain_handle_t *domain_handle);
	int (*domain_close)(uet_domain_handle_t domain_handle);
	int (*endpoint)(uet_domain_handle_t domain_handle, struct fi_info *info,
			struct fid_ep *ep, void *context,
			uet_ep_handle_t *ep_handle);
	int (*getname)(uet_ep_handle_t ep_handle, struct uet_addr *addr);
	int (*ep_bind_cq)(uet_ep_handle_t ep_handle, struct fi_cq_attr *attr,
			  struct fid_cq *cq, uint64_t flags, void *context,
			  uet_cq_handle_t *cq_handle);
	int (*ep_bind_mr)(uet_ep_handle_t ep_handle, uet_mr_handle_t mr_handle,
			  uint64_t flags);
	int (*ep_enable)(uet_ep_handle_t ep_handle);
	int (*ep_control)(uet_ep_handle_t ep_handle, int command, void *arg);
	int (*ep_setopt)(uet_ep_handle_t ep_handle, int level, int optname,
			 const void *optval, size_t optlen);
	int (*ep_close)(uet_ep_handle_t ep_handle);
	int (*cancel)(uet_ep_handle_t ep_handle, void *context);

	ssize_t (*cq_read)(uet_cq_handle_t cq_handle, void *buf, size_t count);
	uint32_t (*cq_read_src_id)(uet_cq_handle_t cq_handle);
	ssize_t (*cq_readerr)(uet_cq_handle_t cq_handle,
			      struct fi_cq_err_entry *buf);
	int (*cq_close)(uet_cq_handle_t cq_handle);

	int (*av_insert)(uet_domain_handle_t domain_handle,
			 struct uet_addr *addr, uet_addr_handle_t *addr_handle);
	int (*av_remove)(uet_addr_handle_t addr_handle);

	int (*mr_reg)(uet_domain_handle_t domain_handle, const void *buf,
		      size_t len, uint64_t access, uint64_t requested_key,
		      uint64_t flags, void *context, uet_mr_handle_t *mr_handle);
	int (*mr_regv)(uet_domain_handle_t domain_handle,
		       const struct iovec *iov, size_t iov_count,
		       uint64_t access, uint64_t requested_key, uint64_t flags,
		       void *context, uet_mr_handle_t *mr_handle);
	uint64_t (*mr_key)(uet_mr_handle_t mr_handle);
	int (*mr_enable)(uet_mr_handle_t mr_handle);
	int (*mr_disable)(uet_mr_handle_t mr_handle);
	int (*mr_close)(uet_mr_handle_t mr_handle);

	ssize_t (*send)(uet_ep_handle_t ep_handle, uint32_t job_id, void *buf,
			size_t len, uet_mr_handle_t mr_handle,
			uet_addr_handle_t dst_addr_handle, void *context);
	ssize_t (*sendv)(uet_ep_handle_t ep_handle, uint32_t job_id,
			 const struct iovec *iov, size_t count,
			 uet_mr_handle_t mr_handle,
			 uet_addr_handle_t dst_addr_handle, void *context);
	ssize_t (*recv)(uet_ep_handle_t ep_handle, uint32_t job_id, void *buf,
			size_t len, uet_mr_handle_t mr_handle,
			uet_addr_handle_t src_addr_handle, void *context);
	ssize_t (*recvv)(uet_ep_handle_t ep_handle, uint32_t job_id,
			 const struct iovec *iov, size_t count,
			 uet_mr_handle_t mr_handle,
			 uet_addr_handle_t src_addr_handle, void *context);
	ssize_t (*tsend)(uet_ep_handle_t ep_handle, uint32_t job_id, void *buf,
			 size_t len, uet_mr_handle_t mr_handle,
			 uet_addr_handle_t dst_addr_handle, uint64_t tag,
			 void *context);
	ssize_t (*tsendv)(uet_ep_handle_t ep_handle, uint32_t job_id,
			  const struct iovec *iov, size_t count,
			  uet_mr_handle_t mr_handle,
			  uet_addr_handle_t dst_addr_handle, uint64_t tag,
			  void *context);
	ssize_t (*trecv)(uet_ep_handle_t ep_handle, uint32_t job_id, void *buf,
			 size_t len, uet_mr_handle_t mr_handle,
			 uet_addr_handle_t src_addr_handle, uint64_t tag,
			 uint64_t ignore, void *context);
	ssize_t (*trecvv)(uet_ep_handle_t ep_handle, uint32_t job_id,
			  const struct iovec *iov, size_t count,
			  uet_mr_handle_t mr_handle,
			  uet_addr_handle_t src_addr_handle, uint64_t tag,
			  uint64_t ignore, void *context);
};

struct uet_fi_fabric {
	struct fid_fabric fabric_fid;
	struct uet_fi_engine engine;
	uet_handle_t uet;
	atomic_uint refs;
	atomic_bool domain_open;
};

struct uet_fi_eq {
	struct fid_eq eq_fid;
	struct uet_fi_fabric *fabric;
};

struct uet_fi_domain {
	struct fid_domain domain_fid;
	struct uet_fi_fabric *fabric;
	uet_domain_handle_t uet_domain;
	struct fi_info *engine_info;
	pthread_mutex_t control_lock;
	atomic_uint refs;
};

struct uet_fi_av_entry {
	struct uet_addr addr;
	uet_addr_handle_t uet_addr;
	bool active;
};

struct uet_fi_av {
	struct fid_av av_fid;
	struct uet_fi_domain *domain;
	struct uet_fi_av_entry **entries;
	size_t count;
	pthread_rwlock_t lock;
	atomic_uint refs;
};

struct uet_fi_ep;

struct uet_fi_cq {
	struct fid_cq cq_fid;
	struct uet_fi_domain *domain;
	struct fi_cq_attr attr;
	struct uet_fi_ep *ep;
	uet_cq_handle_t tx_cq;
	uet_cq_handle_t rx_cq;
	pthread_mutex_t lock;
	bool read_rx_next;
	atomic_uint refs;
};

struct uet_fi_mr {
	struct fid_mr mr_fid;
	struct uet_fi_domain *domain;
	struct uet_fi_ep *ep;
	struct uet_fi_mr *ep_next;
	uet_mr_handle_t uet_mr;
	bool enabled;
};

struct uet_fi_ep {
	struct fid_ep ep_fid;
	struct uet_fi_domain *domain;
	struct fi_info *engine_info;
	uet_ep_handle_t uet_ep;
	struct uet_fi_av *av;
	struct uet_fi_cq *tx_cq;
	struct uet_fi_cq *rx_cq;
	struct uet_fi_mr *mrs;
	bool enabled;
};

#define UET_FI_CONTAINER(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

int uet_fi_engine_load(struct uet_fi_engine *engine);
void uet_fi_engine_unload(struct uet_fi_engine *engine);

int uet_fi_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		       struct fid_domain **domain, void *context);
int uet_fi_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr,
		   struct fid_eq **eq, void *context);
int uet_fi_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
		   struct fid_av **av, void *context);
int uet_fi_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		   struct fid_cq **cq, void *context);
int uet_fi_endpoint_open(struct fid_domain *domain, struct fi_info *info,
			 struct fid_ep **ep, void *context);

int uet_fi_mr_reg(struct fid *fid, const void *buf, size_t len,
		  uint64_t access, uint64_t offset, uint64_t requested_key,
		  uint64_t flags, struct fid_mr **mr, void *context);
int uet_fi_mr_regv(struct fid *fid, const struct iovec *iov, size_t count,
		   uint64_t access, uint64_t offset, uint64_t requested_key,
		   uint64_t flags, struct fid_mr **mr, void *context);
int uet_fi_mr_regattr(struct fid *fid, const struct fi_mr_attr *attr,
		      uint64_t flags, struct fid_mr **mr);

struct uet_fi_av_entry *uet_fi_av_get(struct uet_fi_av *av,
				      fi_addr_t fi_addr);
fi_addr_t uet_fi_av_find_src(struct uet_fi_av *av, uint32_t initiator_id);

#endif /* UET_FI_H */
