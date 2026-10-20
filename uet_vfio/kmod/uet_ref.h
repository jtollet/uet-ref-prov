/* SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB */

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * uet_ref - reference driver for the UET reference PCIe device.
 *
 * The device is the uet-ref-prov libvfio-user model, which presents the
 * Ultra Ethernet reference implementation as a PCIe function. This driver
 * is deliberately monolithic: one module registers both the netdev and
 * (from slice 1) the ib_device, the latter associated with the former.
 * That association is not cosmetic - ib_core derives RoCE-style GID tables
 * from the netdev's addresses, so the UET verbs half has no source of
 * addressing truth without it.
 */

#ifndef _UET_REF_H_
#define _UET_REF_H_

#include <linux/netdevice.h>
#include <linux/pci.h>

#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/mutex.h>

#include <rdma/ib_verbs.h>
#include <rdma/uverbs_ioctl.h>

#include "uet_dev_abi.h"

#define UET_REF_DRV_NAME	"uet_ref"

/*
 * Out-of-tree driver id, in the same spirit as uprot's. A real upstream
 * submission would add RDMA_DRIVER_UET_REF to the enum; until then this
 * keeps the provider and driver agreeing without patching a uapi header.
 */
#define RDMA_DRIVER_UET_REF	((enum rdma_driver_id)0x43)

/*
 * Limits reported by UVERBS_METHOD_DEVICE_QUERY that are the driver's own
 * rather than the device's. There is no admin queue to ask yet, so these
 * are stated here instead of left at zero.
 */
#define UET_REF_MAX_JOB_IDS	256
#define UET_REF_MAX_JOB_KEYS	256
#define UET_REF_MAX_ADDR_ENTRIES 4096

/* one ring's worth of slots; must satisfy the device's power-of-two rule */
#define UET_REF_RING_ENTRIES	64

/* l2 tx/rx, admin queue, and data completions */
#define UET_REF_NUM_VECTORS	4

/* admin queue depth; commands are serialised, so this only bounds bursts */
#define UET_REF_ADMIN_ENTRIES	8
#define UET_REF_ADMIN_SQ_BYTES \
	(UET_REF_ADMIN_ENTRIES * sizeof(struct uet_dev_admin_cmd))
#define UET_REF_ADMIN_CQ_BYTES \
	(UET_REF_ADMIN_ENTRIES * sizeof(struct uet_dev_admin_cqe))
#define UET_REF_ADMIN_MAX_PARAMS	4
#define UET_REF_ADMIN_RESP_BYTES	256

/*
 * Protection domains are the driver's to allocate, not the device's. The
 * device is told a PD only when a region or queue pair is created, and its
 * job is to enforce that binding when data moves.
 */
#define UET_REF_MAX_PDS		1024

/* a ring of descriptors plus the frame buffers they point at */
struct uet_ref_ring {
	struct uet_dev_desc *desc;
	dma_addr_t desc_dma;

	void *buf[UET_REF_RING_ENTRIES];
	dma_addr_t buf_dma[UET_REF_RING_ENTRIES];

	u32 prod; /* ours to advance; published by doorbell */
	u32 cons; /* our view of what the device has consumed */
};

/* verbs objects: nothing driver-private yet, but the core needs the sizes */
/*
 * The largest region this driver can describe, bounded by the contiguous
 * page list a level-1 PBL requires. 256K pages is 1 GiB at 4 KiB pages, for
 * a 2 MiB list.
 */
/*
 * Most pages one region may have.
 *
 * The bound is the root allocation, which must be contiguous: a two-level
 * list needs one root entry per directory and a directory covers
 * PAGE_SIZE/8 pages, so this many pages needs
 * (UET_REF_MR_MAX_PAGES / (PAGE_SIZE/8)) * 8 bytes of root. At 4 KiB pages
 * that is 64 KiB of contiguous memory for a 16 GiB region - against 32 MiB
 * for the same region as a flat list, which is why it used to stop at
 * 1 GiB.
 */
#define UET_REF_MR_MAX_PAGES	(4 * 1024 * 1024)

struct uet_ref_mr {
	struct ib_mr ibmr;
	struct ib_umem *umem;

	     /* The page list the device reads, in memory it can reach.
	      *
	      * One level or two. A one-level list is a flat array of page
	      * addresses and needs npages * 8 bytes of contiguous DMA
	      * memory, which is what used to bound a region to 1 GiB: two
	      * megabytes contiguous is already a lot to ask of the
	      * allocator. A two-level list is a small root of directory
	      * addresses, each directory one page holding page addresses,
	      * so nothing larger than a page is ever allocated for the
	      * leaves and the ceiling moves out by three orders of
	      * magnitude.
	      */
	__le64 *pbl;			/* the root */
	dma_addr_t pbl_dma;
	size_t pbl_bytes;
	size_t npages;
	u32 pbl_level;			/* UET_DEV_PBL_LEVEL_* */
	     /* leaf directories, when the list has two levels */
	__le64 **pbl_dirs;
	dma_addr_t *pbl_dir_dma;
	size_t pbl_ndirs;

	u32 handle;		/* the device's */
	u64 key;		/* the library's 64-bit key */
};

/*
 * A datapath ring: the process allocates it, we pin it and hand the device
 * a page list. Distinct from struct uet_ref_ring, which is the driver's own
 * L2 ring in coherent memory.
 */
struct uet_ref_uring {
	struct ib_umem *umem;
	__le64 *pbl;
	dma_addr_t pbl_dma;
	size_t pbl_bytes;
	u32 npages;
	u32 entries;
};

struct uet_ref_jkey {
	struct ib_jkey ibjkey;
	u32 handle;
};

static inline struct uet_ref_jkey *to_uet_jkey(struct ib_jkey *ibjkey)
{
	return container_of(ibjkey, struct uet_ref_jkey, ibjkey);
}

struct uet_ref_ah {
	struct ib_ah ibah;
	u32 handle;		/* the device's */
};

struct uet_ref_cq {
	struct ib_cq ibcq;
	u32 handle;		/* the device's */
	struct uet_ref_uring ring;
	struct list_head entry;
};

struct uet_ref_qp {
	struct ib_qp ibqp;
	struct ib_jkey *jkey;	/* the job key this pair was created with */
	u32 handle;		/* the device's */
	u32 qpn;		/* [ABS | rsvd | RI | PIDonFEP] */
	u32 requested_qpn;	/* what the application asked for */
	bool qpn_requested;	/* whether it asked; zero is a valid QPN */
	u32 src_id;
	     /* Largest payload this pair puts in a packet, in bytes; zero
	      * means the device's own. Kept so query can report it and
	      * modify can refuse to change it.
	      */
	u32 path_mtu;
	bool absolute;
	     /* enum ib_qp_state. Created in RTS; only RTS, ERR and RESET
	      * are reachable - see uet_ref_modify_qp().
	      */
	u8 state;
	     /* Attributes the application set that this device does not act
	      * on. Recorded rather than discarded so query_qp answers with
	      * what was asked for, which is what uprot does and what a
	      * caller that sets an attribute and reads it back expects.
	      */
	u16 pkey_index;
	u8 port_num;
	int qp_access_flags;
	struct uet_ref_uring sq;
	struct uet_ref_uring rq;
};

struct uet_ref_pd {
	struct ib_pd ibpd;
	u32 pdn;		/* our id, from pd_ida */

	     /* Resource Indices allocated under this domain. They only need
	      * to be unique within it, because the domain's PIDonFEP is the
	      * rest of the address.
	      */
	struct ida ri_ida;

	     /*
	      * A PD is associated with exactly one PIDonFEP, latched by the
	      * first queue pair created under it; every later QP must carry
	      * the same value in its QPN or be rejected. Zero means not yet
	      * latched. Enforcement arrives with queue pair creation.
	      */
	u32 pidonfep;
	bool pidonfep_latched;
};

struct uet_ref_ucontext {
	struct ib_ucontext ibucontext;
	     /* Which BAR3 doorbell page this process may ring. The device
	      * refuses a doorbell whose handle it does not own, so this is
	      * the identity that check is made against.
	      */
	u32 db_page;
	struct rdma_user_mmap_entry *db_entry;
};

struct uet_ref_dev {
	     /* The device is allocated by ib_alloc_device(), which requires
	      * ib_dev to be embedded in this structure
	      */
	struct ib_device ib_dev;
	bool ib_registered;

	struct pci_dev *pdev;
	struct net_device *netdev;

	void __iomem *bar0;	/* status and live counters, read only */
	void __iomem *bar2;	/* doorbells and ring configuration */
	phys_addr_t bar3_phys;	/* user doorbell pages, mapped to processes */
	struct ida db_ida;	/* which of those pages are handed out */

	     /* L2 rings: see uet_ref_netdev.c */
	struct uet_ref_ring tx;
	struct uet_ref_ring rx;

	     /* admin queue: one command at a time, see uet_ref_admin.c */
	struct uet_dev_admin_cmd *admin_sq;
	dma_addr_t admin_sq_dma;
	struct uet_dev_admin_cqe *admin_cq;
	dma_addr_t admin_cq_dma;
	void *admin_resp;
	dma_addr_t admin_resp_dma;
	u32 admin_sq_prod;
	u32 admin_cq_cons;
	u64 admin_cookie;
	struct mutex admin_lock;
	struct completion admin_done;

	     /* what the device told us about itself, read once at probe */
	u64 dev_caps;
	u32 max_job_ids;
	u32 max_job_keys;
	u32 max_addr_entries;
	u32 max_imm_size;

	     /* the pd and PIDonFEP id spaces are ours */
	struct ida pd_ida;
	struct ida pidonfep_ida;

	     /* L2 Tx/Rx, admin queue, and CQ vectors */
	int tx_irq;
	int rx_irq;
	int admin_irq;
	int cq_irq;

	     /* Completion queues that may need telling. The device raises
	      * one vector for all of them, so the handler notifies each;
	      * a consumer re-polls anyway, and the alternative is a vector
	      * per queue.
	      */
	struct list_head cq_list;
	spinlock_t cq_lock;

	bool irqs_ready;
};

/* BAR0 is read only */
static inline u32 uet_ref_rd0(struct uet_ref_dev *dev,
			      u32 off)
{
	return ioread32(dev->bar0 + off);
}

/* BAR2 is write only */
static inline void uet_ref_wr2(struct uet_ref_dev *dev,
			       u32 off,
			       u32 val)
{
	iowrite32(val, dev->bar2 + off);
}

int uet_ref_netdev_probe(struct uet_ref_dev *dev);
void uet_ref_netdev_remove(struct uet_ref_dev *dev);
irqreturn_t uet_ref_tx_isr(int irq, void *data);
irqreturn_t uet_ref_rx_isr(int irq, void *data);

int uet_ref_admin_init(struct uet_ref_dev *dev);
void uet_ref_admin_fini(struct uet_ref_dev *dev);
irqreturn_t uet_ref_admin_isr(int irq, void *data);
int uet_ref_admin_cmd(struct uet_ref_dev *dev, u16 opcode,
		      const u64 *params, unsigned int nparams,
		      void *resp, size_t resp_len, u64 *result);
int uet_ref_admin_query_dev_info(struct uet_ref_dev *dev);

/* jobs, job keys and address tables: uet_ref_job.c */

struct uet_ref_job {
	struct ib_job ibjob;
	u32 handle; /* the device's handle */
};

static inline struct uet_ref_job *to_uet_job(struct ib_job *ibjob)
{
	return container_of(ibjob, struct uet_ref_job, ibjob);
}

int uet_ref_alloc_job(struct ib_job *ibjob, struct ib_job_attr *attr,
		      struct uverbs_attr_bundle *attrs);
void uet_ref_dealloc_job(struct ib_job *ibjob);
int uet_ref_query_job(struct ib_job *ibjob, struct ib_job_attr *attr);
int uet_ref_create_jkey(struct ib_jkey *ibjkey,
			struct uverbs_attr_bundle *attrs);
int uet_ref_destroy_jkey(struct ib_jkey *ibjkey,
			 struct uverbs_attr_bundle *attrs);
int uet_ref_job_addr_insert(struct ib_job *ibjob, u32 index,
			    struct rdma_ah_attr_ex *ah_attr);
int uet_ref_job_addr_remove(struct ib_job *ibjob, u32 index);
int uet_ref_job_addr_query(struct ib_job *ibjob, u32 index,
			   struct rdma_ah_attr_ex *ah_attr);

/* memory regions: uet_ref_mr.c */

struct ib_mr *uet_ref_reg_user_mr(struct ib_pd *ibpd, u64 start, u64 length,
				  u64 virt_addr, int access_flags,
				  struct ib_dmah *dmah,
				  struct ib_udata *udata);
struct ib_mr *uet_ref_reg_user_mr_ex(struct ib_pd *ibpd,
				     struct ib_mr_ex_attr *attr,
				     struct ib_udata *udata);
struct ib_mr *uet_ref_reg_user_mr_dmabuf(struct ib_pd *ibpd, u64 offset,
					 u64 length, u64 virt_addr, int fd,
					 int access_flags,
					 struct ib_dmah *dmah,
					 struct uverbs_attr_bundle *attrs);
int uet_ref_qp_attach_mr(struct ib_qp *ibqp, struct ib_mr *ibmr);
int uet_ref_qp_detach_mr(struct ib_qp *ibqp, struct ib_mr *ibmr);
int uet_ref_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata);

/* address handles: uet_ref_ah.c */

int uet_ref_create_ah(struct ib_ah *ibah, struct rdma_ah_init_attr *init_attr,
		      struct ib_udata *udata);
int uet_ref_destroy_ah(struct ib_ah *ibah, u32 flags);

/* descriptor rings shared by the QP and CQ functions: uet_ref_ring.c */

int uet_ref_uring_pin(struct uet_ref_dev *dev, struct ib_device *ibdev,
		      u64 addr, u32 entries, u32 entry_size,
		      struct uet_ref_uring *ring);
void uet_ref_uring_unpin(struct uet_ref_dev *dev,
			 struct uet_ref_uring *ring);
void uet_ref_uring_desc(struct uet_ref_uring *ring,
			struct uet_dev_ring_desc *desc);

/* completion queues: uet_ref_cq.c */

irqreturn_t uet_ref_cq_isr(int irq, void *data);
int uet_ref_create_cq(struct ib_cq *ibcq, const struct ib_cq_init_attr *attr,
		      struct uverbs_attr_bundle *attrs);
int uet_ref_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata);

/* queue pairs: uet_ref_qp.c */

int uet_ref_create_qp(struct ib_qp *ibqp, struct ib_qp_init_attr *init_attr,
		      struct ib_udata *udata);
int uet_ref_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata);
int uet_ref_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		      int attr_mask, struct ib_udata *udata);
int uet_ref_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		     int attr_mask, struct ib_qp_init_attr *init_attr);

/* statistics: the device's counters, published as ib hw_counters */

extern const struct ib_device_ops uet_ref_stats_ops;
int uet_ref_stats_add_dev_attrs(struct uet_ref_dev *dev);

int uet_ref_verbs_probe(struct uet_ref_dev *dev);
void uet_ref_verbs_remove(struct uet_ref_dev *dev);

#endif /* _UET_REF_H_ */

