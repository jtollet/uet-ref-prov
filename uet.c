/*
 * Copyright (c) 2024,2025,2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * Simple client-server ping-pong application using UET APIs
 *   - supports message and RMA data transfers
 *   - supports both untagged and tagged messages
 *   - supports test option that covers untagged message data transfers,
 *     tagged message data transfers, RMA (read and write) data transfers,
 *     unexpected message handling for untagged/tagged messages, and
 *     deferred send for untagged/tagged messages
 *
 * Description of message operation (applies to both untagged and
 *                                   tagged messages):
 *   - the server is started first with the IP address of the
 *     client as a command line arg
 *   - the client is then started with the IP address of the
 *     server as a command line arg
 *   - the client sends a message to the server
 *   - the server receives the message and then sends it back
 *     to the client
 *
 * Description of RMA operation:
 *   - the server is started first with the IP address of the
 *     client as a command line arg
 *   - the client is then started with the IP address of the
 *     server as a command line arg
 *   - the client sends a control message to the server that identifies the
 *     address and key of its' RMA buffer
 *   - the server receives the control message and then sends a control message
 *     to the client with the address and key of its' RMA buffer
 *   - the client receives the control message and initiates a RMA write of the
 *     data message to the server's buffer
 *     - the write includes completion data that generates a completion
 *       at the server
 *   - the server receives the write completion and writes the data message from
 *     its' buffer to the client's buffer
 *     - the write includes completion data that generates a completion
 *       at the client
 *   - the client receives the write completion to complete the ping pong
 *     data message exchange
 *
 * The number of messages to exchange, UET_NUM_ITERATIONS, is defined at
 * compile time
 *
 * Usage:
 *   ./uet <server | client>
 *	<rma | sync_rma | untag | tag | tag_any_src | unexp_untag | unexp_tag |
 *       defer_send | defer_tag | defer_tag_any_src | atomic |
 *       sync_atomic | all>
 *	<remote IP address>
 *
 *   the penultimate arg specifies the test suite to execute,
 *   'all' executes all of the test suites
 *
 * Server Usage Example:
 *   ./uet server all 192.168.1.18
 *
 * Client Usage Example:
 *   ./uet client all 192.168.1.24
 */

#include <stdio.h>
#include <stdint.h>

#include "uet_api.h"
#include "uet_api_private.h"
#include "uet_sec.h"

#define UET_NUM_ITERATIONS	100
#define UET_ITERATIONS_ENV	"UET_NUM_ITERATIONS"

#define UET_DEFAULT_MSG_SIZE	4096	/* in bytes */
#define UET_MSG_SIZE_ENV	"UET_MSG_SIZE"
#define UET_UUD_MSG_SIZE	1024	/* UUD is single-packet only */
#define UET_UUD_BLAST_N		100	/* datagrams per UUD blast */
#define UET_UUD_RX_TIMEOUT_MS	500	/* per-datagram bounded wait */
#define UET_UUD_FIRST_TIMEOUT_MS 30000	/* server wait for 1st datagram */
#define UET_UUD_DRAIN_QUIET	6	/* empty windows => blast is over */
#define UET_MIN_ATOMIC_MSG_SIZE 24
#define UET_NUM_BUFS		((size_t) 8)
#define UET_DEFAULT_TAG		((uint64_t) 1)
#define UET_WRITE_IMM_DATA	((uint64_t) 0x0CAA)

#define UET_MAX_ARGS		4

/* return codes */
typedef enum {
	UET_ERR_RC     = -1,
	UET_SUCCESS_RC =  0
} uet_rc_t;

#ifndef UET_PACKED
#define UET_PACKED __attribute__((__packed__))
#endif

#define UET_WARN(fmt, ...)                                                     \
	fprintf(stdout, "[%s] %s:%-4d: " fmt "\n", "warning",                  \
		__FILE__, __LINE__, ##__VA_ARGS__)

#define UET_ERR(fmt, ...)                                                      \
	fprintf(stdout, "[%s] %s:%-4d: " fmt "\n", "error",                    \
		__FILE__, __LINE__, ##__VA_ARGS__)

#define UET_PRINT_ERRNO(CALL)                                                  \
	fprintf(stdout, "%s(): %s:%-4d, ret = %d (%s)\n",                      \
		(CALL), __FILE__, __LINE__, errno, strerror(errno))

void UET_USAGE(char *cmd)
{
	fprintf(stdout,
	        "Usage: %s <server|client> <command> <remote IPv4/v6 addr>\n"
	         "  <command>: rma\n"
	         "             sync_rma\n"
	         "             untag\n"
	         "             tag\n"
	         "             tag_any_src\n"
	         "             unexp_untag\n"
	         "             unexp_tag\n"
	         "             defer_send\n"
	         "             defer_tag\n"
	         "             defer_tag_any_src\n"
	         "             atomic\n"
	         "             sync_atomic\n"
	         "             uud\n"
	         "\n"
	         "  PDS delivery-mode overrides (env vars, require UET_PDS=pds):\n"
	         "    UET_FORCE_RUDI=1  force RUDI (reliable unordered, idempotent)\n"
	         "                      on the 'rma' command\n"
	         "    UET_FORCE_UUD=1   force UUD (unreliable datagram, best-effort)\n"
	         "                      on the 'uud' command\n"
	         "\n"
	         "  Memory region overrides (env vars):\n"
	         "    UET_MR_IOV=<n>    register the RMA memory region as <n>\n"
	         "                      iovec entries (uet_mr_regv) instead of\n"
	         "                      one contiguous region. Same bytes, same\n"
	         "                      order - exercises UET_MR_BUF_TYPE_IOV.\n"
	         "                      Applies to rma/sync_rma/atomic tests.\n"
	         "    UET_MR_PBL=<sz>   register the RMA memory region as a page\n"
	         "                      buffer list with <sz> byte pages, as a\n"
	         "                      device driver would. Same bytes, same\n"
	         "                      order. Takes precedence over UET_MR_IOV.\n"
	         "    UET_MR_PBL_LEVEL=<n> PBL level: 0 contiguous, 1 one-level\n"
	         "                      page directory (default), 2 two-level.\n"
	         "    UET_MR_PBL_OFFSET=<n> start the region <n> bytes into its\n"
	         "                      first page, as an unaligned base VA\n"
	         "                      does. Must be less than the page size.\n"
	         "    UET_LOCAL_SEG=<n> describe the LOCAL rma buffer as <n>\n"
	         "                      segments naming memory regions rather\n"
	         "                      than as a process address, using the\n"
	         "                      uet_*seg apis.\n"
	         "    UET_LOCAL_SEG_MRS=<m> spread those segments over <m>\n"
	         "                      distinct regions, so one operation\n"
	         "                      crosses several lkeys as a multi-SGE\n"
	         "                      work request does.\n",
	         cmd);
}

/* config parm struct */
struct uet_cfg {
	char *prog_name;                               /* ptr to program name */
	bool client;                /* true => operate as client, else server */
	bool tag;                              /* true => use tagged messages */
	bool tag_any_src;          /* true => tagged buffers match any source */
	bool rma;                               /* true => use rma operations */
	bool rudi;                              /* true => RMA data over RUDI */
	bool uud;                           /* true => untagged send over UUD */
	bool sync_rma;               /* true => use sync group rma operations */
	bool atomic;                         /* true => use atomic operations */
	bool sync_atomic;         /* true => use sync group atomic operations */
	bool unexpected_msg_test;  /* true => this is unexpected message test */
	bool dsend_test;                        /* true => this is dsend test */
	bool iov_test;                               /* true => test uses iov */
	         /* When > 1, the RMA memory region is registered with        */
	         /* uet_mr_regv() as this many iovec entries carved from the  */
	         /* same buffer, exercising UET_MR_BUF_TYPE_IOV. 0 or 1 keeps */
	         /* the contiguous registration. Set via UET_MR_IOV.          */
	size_t mr_iov_count;
	         /* When non-zero, the RMA memory region is registered with   */
	         /* uet_mr_reg_pbl() using this page size. Set via UET_MR_PBL.*/
	size_t mr_pbl_page_size;
	int mr_pbl_level;              /* PBL level, set via UET_MR_PBL_LEVEL */
	         /* Offset of the region within its first page, exercising    */
	         /* the unaligned base VA a real driver may see. Set via      */
	         /* UET_MR_PBL_OFFSET.                                        */
	size_t mr_pbl_page_offset;
	         /* When > 0, rma operations describe their LOCAL buffer as   */
	         /* this many segments over memory regions instead of as a    */
	         /* process address. Set via UET_LOCAL_SEG.                   */
	size_t local_seg_count;
	         /* Spread those segments across this many distinct regions,  */
	         /* which is the multi-lkey case a single-region design       */
	         /* cannot express. Set via UET_LOCAL_SEG_MRS.                */
	size_t local_seg_mrs;
	int num_iterations;                 /* number of messages to exchange */
	size_t msg_size;                         /* size of messages in bytes */
	char *peer_ip_addr_string;             /* peer ip addr in string form */
	struct uet_fa peer_ip_addr;                             /* host order */
	bool is_ipv6;
	struct uet_addr peer_uet_addr;                    /* peer uet address */
	uint32_t job_id;                                    /* id of this job */
};

/* memory region info */
struct UET_PACKED uet_mem_region_info {
	uint64_t rma_buf_addr; /* RMA buffer address */
	uint64_t key         ; /* key for memory region */
};

/* control message exchanged between client and server */
#define uet_ctrl_msg uet_mem_region_info

/* primary control block struct for application */
struct uet_context {
	struct uet_cfg cfg;
	struct uet_addr local_uet_addr;
	uet_handle_t uet_handle;
	uet_domain_handle_t domain_handle;
	uet_ep_handle_t ep_handle;
	uet_cq_handle_t tx_cq_handle;
	uet_cq_handle_t rx_cq_handle;
	uet_addr_handle_t peer_addr_handle;
	struct uet_fa local_ip_addr;                       /* host byte order */
	struct fid_fabric fabric;
	struct fid_domain domain;
	struct fid_ep ep;
	struct fi_info *info;
	struct fi_rx_attr *rx_attr;
	struct fi_cq_attr cq_attr;
	struct fid_cq cq;
	uint8_t *tx_msg;                      /* ptr to uet tx message buffer */
	uint8_t *rx_msg;                      /* ptr to uet rx message buffer */
	uint8_t *mr_buf;                /* ptr to local memory region bufffer */
	struct iovec *tx_iov;
	uint8_t tx_count;
	struct iovec *rx_iov;
	uint8_t rx_count;
	uet_mr_handle_t mr_handle;          /* handle for local memory region */
	uet_dma_addr_t *pbl_root;      /* page dir (L-1) or dir of dirs (L-2) */
	uet_dma_addr_t **pbl_mid;                   /* L-2 middle directories */
	size_t pbl_mid_cnt;
	        /* The region's pages, individually allocated and mapped to   */
	        /* region page N in a deliberately permuted order, so that a  */
	        /* wrong page index yields wrong data rather than the right   */
	        /* bytes by accident.                                         */
	uint8_t **pbl_pages;
	size_t pbl_page_cnt;
	uint8_t *pbl_base;                       /* L-0 the single allocation */
	        /* local buffer expressed as segments, and the extra regions  */
	        /* those segments name when more than one is asked for        */
	struct uet_mr_seg *local_seg;
	size_t local_seg_count;
	uet_mr_handle_t *extra_mr;
	size_t extra_mr_cnt;
	/* buf addr and key for memory regions */
	struct uet_mem_region_info local_mr, remote_mr;
};

struct uet_context uet_ctx;

bool first;

/* callback function for successful asynchronous event completions */
static void uet_eq_callback(uet_handle_t handle,
			    struct fi_eq_entry *eq_entry)
{
	UET_ERR("Unexpected %s", __func__);
}

/* callback function for asynchronous error events */
static void uet_eq_err_callback(uet_handle_t handle,
				struct fi_eq_err_entry *eq_err_entry)
{
	UET_ERR("Unexpected %s", __func__);
}

/* free resources */
static void uet_free_res(struct uet_context *ctx)
{
	int rc;

	if (ctx->peer_addr_handle != UET_NULL_HANDLE) {
		rc = uet_av_remove(ctx->peer_addr_handle);
		if (rc)
			UET_ERR("uet_av_remove: %s", fi_strerror(-rc));
		ctx->peer_addr_handle = UET_NULL_HANDLE;
	}

	if (ctx->ep_handle != UET_NULL_HANDLE) {
		rc = uet_ep_close(ctx->ep_handle);
		if (rc)
			UET_ERR("uet_endpoint_close: %s", fi_strerror(-rc));
		ctx->ep_handle = UET_NULL_HANDLE;
	}

	if (ctx->tx_cq_handle != UET_NULL_HANDLE) {
		rc = uet_cq_close(ctx->tx_cq_handle);
		if (rc)
			UET_ERR("uet_cq_close: %s", fi_strerror(-rc));
		ctx->tx_cq_handle = UET_NULL_HANDLE;
	}

	if (ctx->rx_cq_handle != UET_NULL_HANDLE) {
		rc = uet_cq_close(ctx->rx_cq_handle);
		if (rc)
			UET_ERR("uet_cq_close: %s", fi_strerror(-rc));
		ctx->rx_cq_handle = UET_NULL_HANDLE;
	}

	if (ctx->mr_handle != UET_NULL_HANDLE) {
		rc = uet_mr_close(ctx->mr_handle);
		if (rc)
			UET_ERR("uet_mr_close: %s", fi_strerror(-rc));
		ctx->mr_handle = UET_NULL_HANDLE;
	}

	if (ctx->domain_handle != UET_NULL_HANDLE) {
		rc = uet_domain_close(ctx->domain_handle);
		if (rc)
			UET_ERR("uet_domain_close: %s", fi_strerror(-rc));
		ctx->domain_handle = UET_NULL_HANDLE;
	}

	if (ctx->info) {
		fi_freeinfo(ctx->info);
		ctx->info = NULL;
	}

	if (ctx->uet_handle != UET_NULL_HANDLE) {
		rc = uet_finalize(ctx->uet_handle);
		if (rc)
			UET_ERR("uet_finalize: %s", fi_strerror(-rc));
		ctx->uet_handle = UET_NULL_HANDLE;
	}

	if (ctx->info) {
		fi_freeinfo(ctx->info);
		ctx->info = NULL;
	}

	if (ctx->tx_msg) {
		free(ctx->tx_msg);
		ctx->tx_msg = NULL;
	}

	if (ctx->rx_msg) {
		free(ctx->rx_msg);
		ctx->rx_msg = NULL;
	}

	if (ctx->local_seg) {
		free(ctx->local_seg);
		ctx->local_seg = NULL;
		ctx->local_seg_count = 0;
	}

	if (ctx->extra_mr) {
		free(ctx->extra_mr);
		ctx->extra_mr = NULL;
		ctx->extra_mr_cnt = 0;
	}

	if (ctx->pbl_mid) {
		size_t i;

		for (i = 0; i < ctx->pbl_mid_cnt; i++)
			free(ctx->pbl_mid[i]);

		free(ctx->pbl_mid);
		ctx->pbl_mid = NULL;
		ctx->pbl_mid_cnt = 0;
	}

	if (ctx->pbl_root) {
		free(ctx->pbl_root);
		ctx->pbl_root = NULL;
	}

	if (ctx->pbl_pages) {
		size_t i;

		for (i = 0; i < ctx->pbl_page_cnt; i++)
			free(ctx->pbl_pages[i]);

		free(ctx->pbl_pages);
		ctx->pbl_pages = NULL;
		ctx->pbl_page_cnt = 0;
	}

	if (ctx->pbl_base) {
		free(ctx->pbl_base);
		ctx->pbl_base = NULL;
		ctx->mr_buf = NULL;
	}

	if (ctx->mr_buf) {
		free(ctx->mr_buf);
		ctx->mr_buf = NULL;
	}

	if (ctx->tx_iov) {

		for (int i = 0; i < ctx->tx_count; i++)
			free(ctx->tx_iov[i].iov_base);

		free(ctx->tx_iov);
		ctx->tx_iov = NULL;
	}

	if (ctx->rx_iov) {

		for (int i = 0; i < ctx->rx_count; i++)
			free(ctx->rx_iov[i].iov_base);

		free(ctx->rx_iov);
		ctx->rx_iov = NULL;
	}
}

/* initialize config parms */
static uet_rc_t uet_init_cfg(int argc, char *argv[],
			     struct uet_context *ctx)
{
	struct in_addr peer_in_addr;
	struct in6_addr peer_in6_addr;
	struct uet_addr *addr;
	char *env_iters;
	char *env_msg_size;

	ctx->cfg.job_id = UET_DEF_JOB_ID;

	if ((argc != 3) && (argc != 4))  {
		UET_ERR("Invalid usage: wrong number of args");
		return UET_ERR_RC;
	}

	if (strcmp(argv[1], "client") == 0) {
		ctx->cfg.client = true;
	} else if (strcmp(argv[1], "server") != 0) {
		UET_ERR("Invalid usage: 1st arg must be 'client' "
			"or 'server'");
		return UET_ERR_RC;
	}

	ctx->cfg.prog_name = argv[0];

	/* When RUDI is forced (using UET_FORCE_RUDI), RMA carries the bulk
	 * data over RUDI (idempotent DDP) and once that completes, signals
	 * completion with a separate WRITE w/ Imm over RUD.
	 */
	ctx->cfg.rudi = (getenv("UET_FORCE_RUDI") != NULL);

	/* When UUD is forced (using UET_FORCE_UUD), the untagged send rides
	 * the best-effort datagram mode. UUD is single-packet only, so the
	 * message size is capped to a single packet.
	 */
	ctx->cfg.uud = (getenv("UET_FORCE_UUD") != NULL);

	/* When UET_MR_IOV=<n> is set with n > 1, the RMA memory region is
	 * registered with uet_mr_regv() as n iovec entries carved from the
	 * same buffer instead of one contiguous region.
	 */
	{
		const char *mr_iov = getenv("UET_MR_IOV");

		ctx->cfg.mr_iov_count =
			(mr_iov != NULL) ?
				(size_t)strtoul(mr_iov, NULL, 0) :
				0;
	}

	/* When UET_MR_PBL=<page_size> is set, the RMA memory region is
	 * registered with uet_mr_reg_pbl() as a page buffer list. The pages
	 * are separately allocated and deliberately permuted, so a resolver
	 * that computes the wrong page index returns the wrong bytes rather
	 * than accidentally-correct adjacent ones.
	 */
	{
		const char *pbl = getenv("UET_MR_PBL");
		const char *lvl = getenv("UET_MR_PBL_LEVEL");
		const char *po  = getenv("UET_MR_PBL_OFFSET");

		ctx->cfg.mr_pbl_page_size =
			(pbl != NULL) ?
				(size_t)strtoul(pbl, NULL, 0) :
				0;

		ctx->cfg.mr_pbl_level =
			(lvl != NULL) ?
				(int)strtol(lvl, NULL, 0) :
				UET_PBL_LEVEL_1;

		ctx->cfg.mr_pbl_page_offset =
			(po != NULL) ?
				(size_t)strtoul(po, NULL, 0) :
				0;
	}

	/* When UET_LOCAL_SEG=<n> is set, rma operations describe their local
	 * buffer as n segments naming memory regions, rather than as a
	 * process address. UET_LOCAL_SEG_MRS=<m> spreads those segments over
	 * m distinct regions, so one operation crosses several lkeys.
	 */
	{
		const char *ls = getenv("UET_LOCAL_SEG");
		const char *lm = getenv("UET_LOCAL_SEG_MRS");

		ctx->cfg.local_seg_count =
			(ls != NULL) ?
				(size_t)strtoul(ls, NULL, 0) :
				0;

		ctx->cfg.local_seg_mrs =
			(lm != NULL) ?
				(size_t)strtoul(lm, NULL, 0) :
				1;
	}

	if (argc == 4) {
		if (strcmp(argv[2], "tag") != 0) {
			if ((strcmp(argv[2], "rma") != 0) &&
			    (strcmp(argv[2], "sync_rma") != 0) &&
			    (strcmp(argv[2], "atomic") != 0) &&
			    (strcmp(argv[2], "sync_atomic") != 0)) {
				UET_ERR("Invalid usage: bad test arg");
				return UET_ERR_RC;
			}
			if (strcmp(argv[2], "rma") == 0)
				ctx->cfg.rma = true;
			else if (strcmp(argv[2], "sync_rma") == 0)
				ctx->cfg.sync_rma = true;
			else if (strcmp(argv[2], "atomic") == 0)
				ctx->cfg.atomic = true;
			else
				ctx->cfg.sync_atomic = true;
		} else {
			ctx->cfg.tag = true;
		}
		ctx->cfg.peer_ip_addr_string = argv[3];
	} else {
		ctx->cfg.peer_ip_addr_string = argv[2];
	}

	memset(&ctx->cfg.peer_ip_addr, 0, sizeof(struct uet_fa));

	if (inet_pton(AF_INET6, ctx->cfg.peer_ip_addr_string,
		      &peer_in6_addr) == 1) {
		memcpy(ctx->cfg.peer_ip_addr.v6, &peer_in6_addr, 16);
		ctx->cfg.is_ipv6 = true;
	} else if (inet_pton(AF_INET, ctx->cfg.peer_ip_addr_string,
			     &peer_in_addr) == 1) {
		ctx->cfg.peer_ip_addr.v4 = ntohl(peer_in_addr.s_addr);
		ctx->cfg.is_ipv6 = false;
	} else {
		UET_PRINT_ERRNO("Invalid usage: bad IP address");
		return UET_ERR_RC;
	}

	addr = &ctx->cfg.peer_uet_addr;
	addr->ver = UET_ADDR_VERSION;
	addr->flags = (UET_ADDR_FEP_CAP_V     |
		       UET_ADDR_FA_V          |
		       UET_ADDR_PID_ON_FEP_V  |
		       UET_ADDR_INDEX_V       |
		       UET_ADDR_INITIATOR_V   |
		       UET_ADDR_RELATIVE_MODE |
		       (ctx->cfg.is_ipv6 ? UET_ADDR_IPV6 : UET_ADDR_IPV4) |
		       UET_ADDR_BIG_MSG_SIZE);
	/* advertise HPC profile so the peer may select RUDI (MUST for HPC) */
	addr->fep_cap = (UET_FEP_CAP_AI_FULL | UET_FEP_CAP_HPC);
	memcpy(&addr->fa, &ctx->cfg.peer_ip_addr, sizeof(struct uet_fa));
	addr->pid_on_fep = UET_ADDR_DEF_PID_ON_FEP;
	addr->num_indices = 1;
	addr->start_index = UET_ADDR_DEF_INDEX;
	addr->initiator_id = UET_ADDR_DEF_INITIATOR_ID;

	ctx->cfg.num_iterations = UET_NUM_ITERATIONS;

	/* harness override the number of iterations executed */
	env_iters = getenv(UET_ITERATIONS_ENV);
	if (env_iters != NULL) {
		int n = atoi(env_iters);
		if (n > 0)
			ctx->cfg.num_iterations = n;
	}

	if (ctx->cfg.uud) {
		ctx->cfg.msg_size = UET_UUD_MSG_SIZE;
		ctx->cfg.num_iterations = 1;
	} else if (ctx->cfg.dsend_test) {
		if (ctx->cfg.tag)
			ctx->cfg.msg_size = UET_TAG_RENDEZVOUS_SIZE * 2;
		else
			ctx->cfg.msg_size = UET_MSG_RENDEZVOUS_SIZE * 2;
	} else {
		ctx->cfg.msg_size = UET_DEFAULT_MSG_SIZE;
		env_msg_size = getenv(UET_MSG_SIZE_ENV);
		if (env_msg_size != NULL) {
			char *end;
			unsigned long long n;

			errno = 0;
			n = strtoull(env_msg_size, &end, 10);
			if (errno || (end == env_msg_size) || (*end != '\0') ||
			    (n == 0) || (n > SIZE_MAX)) {
				UET_ERR("Invalid %s=%s", UET_MSG_SIZE_ENV,
					env_msg_size);
				return UET_ERR_RC;
			}
			ctx->cfg.msg_size = (size_t)n;
		}
	}

	return UET_SUCCESS_RC;
}

/* initialize message buffer contents */
static void uet_init_msg_buf(struct uet_context *ctx, uint8_t *buf)
{
	size_t i;

	for (i = 0; i < ctx->cfg.msg_size; i++)
		buf[i] = (uint8_t) i;
}

/* initialize IO vector`s buffer contents */
static void uet_init_iov_buf(size_t size, uint8_t *buf)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = (uint8_t) i;
}


/*
 * Move the region's contents between the linear shadow buffer and the
 * scattered pages the page list actually describes.
 *
 * When the pages are separately allocated and permuted, mr_buf is no longer
 * the region - it is only a linear image of it, used to lay down the test
 * pattern and to verify what came back. Everything the provider touches
 * goes through the pages.
 */
static void uet_pbl_sync(struct uet_context *ctx, bool to_pages)
{
	size_t ps = ctx->cfg.mr_pbl_page_size;
	size_t off = ctx->cfg.mr_pbl_page_offset;
	size_t done = 0;

	if (ctx->pbl_pages == NULL)
		return; /* level 0: the region is mr_buf itself */

	while (done < ctx->cfg.msg_size) {
		size_t abs = (off + done);
		size_t run = (ps - (abs % ps));
		uint8_t *p = (ctx->pbl_pages[abs / ps] + (abs % ps));

		if (run > (ctx->cfg.msg_size - done))
			run = (ctx->cfg.msg_size - done);

		if (to_pages)
			memcpy(p, (ctx->mr_buf + done), run);
		else
			memcpy((ctx->mr_buf + done), p, run);

		done += run;
	}
}

/* validate message buffer contents */
static uet_rc_t uet_validate_msg(struct uet_context *ctx, uint8_t *buf)
{
	size_t i;

	/* The rma target is the page list, not this buffer, refresh the
	 * linear image from the pages before checking it.
	 */
	if (buf == ctx->mr_buf)
		uet_pbl_sync(ctx, false);

	for (i = 0; i < ctx->cfg.msg_size; i++) {
		if (buf[i] != (uint8_t)i)
			return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

static uet_rc_t uet_validate_iov_msg(struct uet_context *ctx)
{
	size_t rx_idx = 0, tx_idx = 0;
	size_t rx_offset = 0, tx_offset = 0;
	uint8_t *rx_buf = ctx->rx_iov[rx_idx].iov_base;
	uint8_t *tx_buf = ctx->tx_iov[tx_idx].iov_base;

	while (rx_idx < ctx->rx_count && tx_idx < ctx->tx_count) {
		if (rx_buf[rx_offset] != tx_buf[tx_offset])
			return UET_ERR_RC;

		rx_offset++;
		tx_offset++;
		if (rx_offset >= ctx->rx_iov[rx_idx].iov_len) {
			rx_offset = 0;
			rx_idx++;
			rx_buf = ctx->rx_iov[rx_idx].iov_base;
		}
		if (tx_offset >= ctx->tx_iov[tx_idx].iov_len) {
			tx_offset = 0;
			tx_idx++;
			tx_buf = ctx->tx_iov[tx_idx].iov_base;
		}
	}

	/* Ensure both buffers were fully validated */
	if (rx_idx < ctx->rx_count || tx_idx < ctx->tx_count)
		return UET_ERR_RC;

	return UET_SUCCESS_RC;
}

/* initialize uet transport */
static uet_rc_t uet_init_transport(struct uet_context *ctx)
{
	int ret;
	char ip_addr_str[INET6_ADDRSTRLEN];
	void *context = NULL;
	struct fi_info *hints = NULL, *info;
	struct uet_addr *uet_addr;
	struct uet_addr src_node;
	uet_rc_t rc = UET_ERR_RC;
	ssize_t remaining_size;

	/* FIXME: Hack used to configure client/server security mode */
	if (!ctx->cfg.client)
		setenv(UET_SEC_SERVER, "1", 1);

	ret = uet_initialize(&ctx->uet_handle);
	if (ret) {
		UET_ERR("uet_initialize: %s", fi_strerror(-ret));
		goto exit;
	}

	hints = fi_allocinfo();
	if (hints == NULL) {
		UET_ERR("fi_allocinfo");
		goto exit;
	}

	if (ctx->cfg.tag)
		hints->caps |= FI_TAGGED;
	else if (ctx->cfg.rma || ctx->cfg.sync_rma)
		hints->caps |= (FI_MSG | FI_RMA);
	else if (ctx->cfg.atomic || ctx->cfg.sync_atomic)
		hints->caps |= (FI_ATOMIC | FI_RMA);
	else
		hints->caps |= FI_MSG;

	/* dual-stack: tell the library which address family this run uses */
	memset(&src_node, 0, sizeof(src_node));
	src_node.flags = ctx->cfg.is_ipv6 ? UET_ADDR_IPV6 : UET_ADDR_IPV4;

	ret = uet_getinfo(ctx->uet_handle, &src_node, hints, &ctx->info);
	if (ret) {
		UET_ERR("uet_getinfo: %s", fi_strerror(-ret));
		goto exit;
	}

	for (info = ctx->info; info; info = info->next) {
		printf("UET device name:   %s\n",
		       info->nic->device_attr->name);
		printf("  Network type:    %s\n",
		       info->nic->link_attr->network_type);
		printf("  MAC Address:     %s\n",
		       info->nic->link_attr->address);
		printf("  MTU:             %ld\n",
		       info->nic->link_attr->mtu);
		printf("  Interface state: ");
		if (info->nic->link_attr->state == FI_LINK_UP)
			printf("UP\n");
		else if (info->nic->link_attr->state == FI_LINK_DOWN)
			printf("DOWN\n");
		else
			printf("UNKNOWN\n");
		uet_addr = (struct uet_addr *) info->src_addr;
		uet_ip_addr_to_str(&uet_addr->fa,
				   uet_addr_is_ipv6(uet_addr),
				   ip_addr_str);
		printf("  IP Address:      %s\n", ip_addr_str);
	}

	if (ctx->cfg.msg_size > ctx->info->ep_attr->max_msg_size) {
		UET_ERR("Max message size too small: %lu", ctx->cfg.msg_size);
		goto exit;
	}

	if (ctx->cfg.tag) {
		if (!(ctx->info->caps & FI_TAGGED)) {
			UET_ERR("Tagged messages not supported");
			goto exit;
		}
	} else {
		if (ctx->cfg.rma || ctx->cfg.sync_rma) {
			if (!(ctx->info->caps & FI_RMA)) {
				UET_ERR("RMA operations not supported");
				goto exit;
			}
		}
		if (ctx->cfg.atomic || ctx->cfg.sync_atomic) {
			if (!(ctx->info->caps & FI_ATOMIC)) {
				UET_ERR("Atomic operations not supported");
				goto exit;
			}
		}
		if (!(ctx->info->caps & FI_MSG)) {
			UET_ERR("Message operations not supported");
			goto exit;
		}
	}


	ret = uet_domain(ctx->uet_handle, &ctx->fabric, ctx->info,
			 &ctx->domain, context, uet_eq_callback,
			 uet_eq_err_callback, &ctx->domain_handle);
	if (ret) {
		UET_ERR("uet_domain: %s", fi_strerror(-ret));
		goto exit;
	}

	if (ctx->cfg.rma || ctx->cfg.sync_rma ||
	    ctx->cfg.atomic || ctx->cfg.sync_atomic) {
		ctx->mr_buf = malloc(ctx->cfg.msg_size);
		if (ctx->mr_buf == NULL) {
			UET_PRINT_ERRNO("malloc");
			UET_ERR("Error allocating memory region buffer");
			goto exit;
		}

		if (!ctx->cfg.client)
			uet_init_msg_buf(ctx, ctx->mr_buf);

		ctx->info->domain_attr->mr_mode |= FI_MR_PROV_KEY;

		/* RUDI targets the MR for idempotent RMA so mark it as
		 * IDEMPOTENT_SAFE. The provider still assigns the key but
		 * preserves this flag.
		 */

		if (ctx->cfg.mr_pbl_page_size > 0) {
			size_t ps = ctx->cfg.mr_pbl_page_size;
			size_t po = ctx->cfg.mr_pbl_page_offset;
			size_t npages = (po + ctx->cfg.msg_size + ps - 1) / ps;
			size_t per_dir = ps / sizeof(uet_dma_addr_t);
			uet_dma_addr_t root;
			uint8_t **raw = NULL;
			size_t i;

			if (po >= ps) {
				UET_ERR("UET_MR_PBL_OFFSET must be < page size");
				goto exit;
			}

			if (ctx->cfg.mr_pbl_level == UET_PBL_LEVEL_0) {
				/*
				 * level 0 is contiguous by definition, so the
				 * region is one allocation and page_offset is
				 * simply where it starts inside it
				 */
				ctx->pbl_base = calloc(1, po +
						       ctx->cfg.msg_size);
				if (ctx->pbl_base == NULL) {
					UET_ERR("Error allocating PBL region");
					goto exit;
				}
				memcpy(ctx->pbl_base + po, ctx->mr_buf,
				       ctx->cfg.msg_size);
				free(ctx->mr_buf);
				ctx->mr_buf = ctx->pbl_base + po;
				root = (uet_dma_addr_t) (uintptr_t)
					ctx->pbl_base;
			} else {
				/*
				 * Allocate every page separately, then map
				 * region page N to allocation (npages-1-N).
				 * The permutation is what makes this a real
				 * test: if the resolver computes the wrong
				 * page index it returns the wrong bytes
				 * instead of adjacent - and therefore
				 * accidentally correct - ones.
				 */
				raw = calloc(npages, sizeof(uint8_t *));
				ctx->pbl_pages = calloc(npages,
							sizeof(uint8_t *));
				if ((raw == NULL) || (ctx->pbl_pages == NULL)) {
					free(raw);
					UET_ERR("Error allocating PBL pages");
					goto exit;
				}
				ctx->pbl_page_cnt = npages;

				for (i = 0; i < npages; i++) {
					raw[i] = calloc(1, ps);
					if (raw[i] == NULL) {
						free(raw);
						UET_ERR(
						"Error allocating PBL page");
						goto exit;
					}
				}
				for (i = 0; i < npages; i++)
					ctx->pbl_pages[i] =
						raw[npages - 1 - i];
				free(raw);

				/* lay the region's contents into the pages */
				uet_pbl_sync(ctx, true);

				if (ctx->cfg.mr_pbl_level == UET_PBL_LEVEL_1) {
					ctx->pbl_root = calloc(npages,
						sizeof(uet_dma_addr_t));
					if (ctx->pbl_root == NULL) {
						UET_ERR(
						"Error allocating PBL dir");
						goto exit;
					}
					for (i = 0; i < npages; i++)
						ctx->pbl_root[i] =
							(uet_dma_addr_t)
							(uintptr_t)
							ctx->pbl_pages[i];
				} else if (ctx->cfg.mr_pbl_level ==
					   UET_PBL_LEVEL_2) {
					size_t ndirs = (npages + per_dir - 1) /
						       per_dir;

					ctx->pbl_root = calloc(ndirs,
						sizeof(uet_dma_addr_t));
					ctx->pbl_mid = calloc(ndirs,
						sizeof(uet_dma_addr_t *));
					if ((ctx->pbl_root == NULL) ||
					    (ctx->pbl_mid == NULL)) {
						UET_ERR(
						"Error allocating PBL dirs");
						goto exit;
					}
					ctx->pbl_mid_cnt = ndirs;
					for (i = 0; i < ndirs; i++) {
						ctx->pbl_mid[i] = calloc(1, ps);
						if (ctx->pbl_mid[i] == NULL) {
							UET_ERR(
							"Error alloc PBL dir");
							goto exit;
						}
						ctx->pbl_root[i] =
							(uet_dma_addr_t)
							(uintptr_t)
							ctx->pbl_mid[i];
					}
					for (i = 0; i < npages; i++)
						ctx->pbl_mid[i / per_dir]
							   [i % per_dir] =
							(uet_dma_addr_t)
							(uintptr_t)
							ctx->pbl_pages[i];
				} else {
					UET_ERR("Invalid UET_MR_PBL_LEVEL");
					goto exit;
				}

				root = (uet_dma_addr_t) (uintptr_t)
					ctx->pbl_root;
			}

			printf("Registering MR as a level-%d PBL "
			       "(%zu pages of %zu bytes, page_offset %zu%s)\n",
			       ctx->cfg.mr_pbl_level, npages, ps, po,
			       (ctx->pbl_pages != NULL) ?
					", pages permuted" : "");

			/* base_va 0 makes the region zero-based, so the
			 * addresses naming it are offsets
			 */
			ret = uet_mr_reg_pbl(ctx->domain_handle, root,
					     (uint32_t) ps,
					     ctx->cfg.mr_pbl_level,
					     (uint32_t) po,
					     0 /* base_va */,
					     ctx->cfg.msg_size,
					     FI_WRITE | FI_REMOTE_WRITE |
					     FI_READ  | FI_REMOTE_READ |
					     FI_ATOMIC,
					     ctx->cfg.rudi ?
						UET_MR_KEY_IDEMPOTENT_SAFE :
						UET_MR_KEY_NONE,
					     UET_FLAGS_NONE, context,
					     &ctx->mr_handle);
		} else if (ctx->cfg.mr_iov_count > 1) {
			struct iovec *mr_iov;
			size_t i, off, chunk;
			size_t n = ctx->cfg.mr_iov_count;

			/* cannot carve more entries than there are bytes */
			if (n > ctx->cfg.msg_size)
				n = ctx->cfg.msg_size;

			mr_iov = calloc(n, sizeof(struct iovec));
			if (mr_iov == NULL) {
				UET_PRINT_ERRNO("calloc");
				UET_ERR("Error allocating MR io vector");
				goto exit;
			}

			/*
			 * carve the same buffer into n adjacent slices - the
			 * bytes and their order are unchanged, so all existing
			 * data verification still applies
			 */
			chunk = ctx->cfg.msg_size / n;
			off = 0;
			for (i = 0; i < n; i++) {
				mr_iov[i].iov_base = ctx->mr_buf + off;
				/* last entry absorbs any remainder */
				mr_iov[i].iov_len = (i == (n - 1)) ?
					(ctx->cfg.msg_size - off) : chunk;
				off += mr_iov[i].iov_len;
			}

			printf("Registering MR as %zu iovec entries (%zu bytes)\n",
			       n, ctx->cfg.msg_size);

			/* uet_mr_regv() copies the vector, so it need not
			 * outlive this call
			 */
			ret = uet_mr_regv(ctx->domain_handle, mr_iov, n,
					  FI_WRITE | FI_REMOTE_WRITE |
					  FI_READ  | FI_REMOTE_READ | FI_ATOMIC,
					  ctx->cfg.rudi ?
						UET_MR_KEY_IDEMPOTENT_SAFE :
						UET_MR_KEY_NONE,
					  UET_FLAGS_NONE, context,
					  &ctx->mr_handle);
			free(mr_iov);
		} else {
			ret = uet_mr_reg(ctx->domain_handle, ctx->mr_buf,
					 ctx->cfg.msg_size,
					 FI_WRITE | FI_REMOTE_WRITE |
					 FI_READ  | FI_REMOTE_READ | FI_ATOMIC,
					 ctx->cfg.rudi ?
						UET_MR_KEY_IDEMPOTENT_SAFE :
						UET_MR_KEY_NONE,
					 UET_FLAGS_NONE, context,
					 &ctx->mr_handle);
		}
		if (ret) {
			UET_ERR("uet_mr_reg%s: %s",
				(ctx->cfg.mr_pbl_page_size > 0) ? "_pbl" :
				((ctx->cfg.mr_iov_count > 1) ? "v" : ""),
				fi_strerror(-ret));
			goto exit;
		}

		ctx->local_mr.rma_buf_addr = 0;
		ctx->local_mr.key = uet_mr_key(ctx->mr_handle);
	}

	ctx->info->rx_attr->size = UET_NUM_BUFS;
	ctx->info->tx_attr->size = UET_NUM_BUFS;
	if (ctx->cfg.unexpected_msg_test)
		ctx->info->tx_attr->msg_order = FI_ORDER_NONE;
	else
		ctx->info->tx_attr->msg_order = FI_ORDER_SAS;

	ret = uet_endpoint(ctx->domain_handle, ctx->info, &ctx->ep,
			   context, &ctx->ep_handle);
	if (ret) {
		UET_ERR("uet_endpoint: %s", fi_strerror(-ret));
		goto exit;
	}

	ret = uet_getname(ctx->ep_handle, &ctx->local_uet_addr);
	if (ret) {
		UET_ERR("uet_getname: %s", fi_strerror(-ret));
		goto exit;
	}

	uet_print_uet_addr(&ctx->local_uet_addr);

	if (ctx->cfg.rma || ctx->cfg.sync_rma ||
	    ctx->cfg.atomic || ctx->cfg.sync_atomic) {
		ret = uet_ep_bind_mr(ctx->ep_handle, ctx->mr_handle,
				     UET_FLAGS_NONE);
		if (ret) {
			UET_ERR("uet_ep_bind_mr: %s", fi_strerror(-ret));
			goto exit;
		}
	}

	ret = uet_av_insert(ctx->domain_handle, &ctx->cfg.peer_uet_addr,
			    &ctx->peer_addr_handle);
	if (ret) {
		UET_ERR("uet_av_insert: %s", fi_strerror(-ret));
		goto exit;
	}

	ctx->cq_attr.format = FI_CQ_FORMAT_DATA;
	ctx->cq_attr.size = UET_NUM_BUFS * 2;
	ret = uet_ep_bind_cq(ctx->ep_handle, &ctx->cq_attr, &ctx->cq,
			     FI_SEND, context, &ctx->tx_cq_handle);
	if (ret) {
		UET_ERR("uet_cq_bind: %s", fi_strerror(-ret));
		goto exit;
	}

	ret = uet_ep_bind_cq(ctx->ep_handle, &ctx->cq_attr, &ctx->cq,
			     FI_RECV, context, &ctx->rx_cq_handle);
	if (ret) {
		UET_ERR("uet_cq_bind: %s", fi_strerror(-ret));
		goto exit;
	}

	if (ctx->cfg.rma || ctx->cfg.sync_rma ||
	    ctx->cfg.atomic || ctx->cfg.sync_atomic) {
		ret = uet_mr_enable(ctx->mr_handle);
		if (ret) {
			UET_ERR("uet_mr_enable: %s", fi_strerror(-ret));
			goto exit;
		}

		/*
		 * Describe the local rma buffer as a segment list instead of
		 * a process address. The segments carve the same region so
		 * the bytes and their order are unchanged and every existing
		 * verification still applies (only the way the buffer is
		 * named changes. With UET_LOCAL_SEG_MRS the carve is spread
		 * over several separately registered regions covering the
		 * same memory, so one operation genuinely crosses several
		 * lkeys, which is what a multi-SGE work request does.
		 */
		if (ctx->cfg.local_seg_count > 0) {
			size_t nseg = ctx->cfg.local_seg_count;
			size_t nmr = ctx->cfg.local_seg_mrs;
			size_t chunk, off, i;

			/*
			 * sync_rma issues partial writes straight from
			 * mr_buf, which is only a linear shadow of the region
			 * when the region is a page list. Segment operations
			 * land in the pages themselves, so mixing the two
			 * leaves the shadow stale and the data check fails on
			 * a harness artifact rather than a real defect.
			 *
			 * Each half is fine on its own - segments with a
			 * contiguous region, or a page list without segments -
			 * so refuse only the combination, and say why.
			 */
			if ((ctx->cfg.mr_pbl_page_size > 0) &&
			    ctx->cfg.sync_rma) {
				UET_ERR("UET_LOCAL_SEG with UET_MR_PBL is "
					"not supported for sync_rma:");
				UET_ERR("  sync_rma writes partial buffers "
					"directly from the shadow buffer,");
				UET_ERR("  which segment operations bypass. "
					"Use one or the other.");
				goto exit;
			}

			if (nseg > ctx->cfg.msg_size)
				nseg = ctx->cfg.msg_size;
			if (nmr < 1)
				nmr = 1;
			if (nmr > nseg)
				nmr = nseg;

			ctx->local_seg = calloc(nseg,
						sizeof(struct uet_mr_seg));
			if (ctx->local_seg == NULL) {
				UET_ERR("Error allocating local segments");
				goto exit;
			}
			ctx->local_seg_count = nseg;

			/*
			 * Extra regions beyond the first each cover the whole
			 * buffer; a segment then names a slice of whichever
			 * region it was assigned. Registering the same memory
			 * more than once is exactly what distinct lkeys over
			 * overlapping memory look like.
			 */
			if (nmr > 1) {
				ctx->extra_mr = calloc(nmr - 1,
						sizeof(uet_mr_handle_t));
				if (ctx->extra_mr == NULL) {
					UET_ERR("Error allocating extra MRs");
					goto exit;
				}

				for (i = 0; i < (nmr - 1); i++) {
					ret = uet_mr_reg(ctx->domain_handle,
						ctx->mr_buf, ctx->cfg.msg_size,
						FI_WRITE | FI_REMOTE_WRITE |
						FI_READ | FI_REMOTE_READ |
						FI_ATOMIC,
						UET_MR_KEY_NONE,
						UET_FLAGS_NONE, context,
						&ctx->extra_mr[i]);
					if (ret == 0)
						ret = uet_ep_bind_mr(
							ctx->ep_handle,
							ctx->extra_mr[i],
							UET_FLAGS_NONE);
					if (ret == 0)
						ret = uet_mr_enable(
							ctx->extra_mr[i]);
					if (ret) {
						UET_ERR("extra MR %zu: %s", i,
							fi_strerror(-ret));
						goto exit;
					}
					ctx->extra_mr_cnt++;
				}
			}

			chunk = ctx->cfg.msg_size / nseg;
			off = 0;
			for (i = 0; i < nseg; i++) {
				size_t which = i % nmr;

				ctx->local_seg[i].mr = (which == 0) ?
					ctx->mr_handle :
					ctx->extra_mr[which - 1];
				ctx->local_seg[i].addr = off;
				ctx->local_seg[i].len = (i == (nseg - 1)) ?
					(ctx->cfg.msg_size - off) : chunk;
				off += ctx->local_seg[i].len;
			}

			printf("Local rma buffer as %zu segments "
			       "over %zu region(s)\n",
			       nseg, nmr);
		}
	}

	ret = uet_ep_enable(ctx->ep_handle);
	if (ret) {
		UET_ERR("uet_endpoint_enable: %s", fi_strerror(-ret));
		goto exit;
	}

	ctx->tx_msg = malloc(ctx->cfg.msg_size);
	if (ctx->tx_msg == NULL) {
		UET_PRINT_ERRNO("malloc");
		UET_ERR("Error allocating message buffer");
		goto exit;
	}

	if (!ctx->cfg.rma && !ctx->cfg.sync_rma &&
            !ctx->cfg.atomic && !ctx->cfg.sync_atomic && ctx->cfg.client)
		uet_init_msg_buf(ctx, ctx->tx_msg);

	ctx->rx_msg = malloc(ctx->cfg.msg_size);
	if (ctx->rx_msg == NULL) {
		UET_PRINT_ERRNO("malloc");
		UET_ERR("Error allocating message buffer");
		goto exit;
	}

	/* Allocate IO Vectors for Transmit */
	ctx->tx_iov = calloc(UET_IOV_LIMIT_MAX, sizeof(struct iovec));
	if (ctx->tx_iov == NULL) {
		UET_PRINT_ERRNO("calloc");
		UET_ERR("Error allocating IO vector");
		goto exit;
	}
	remaining_size = ctx->cfg.msg_size;
	while (remaining_size > 0 && ctx->tx_count < UET_IOV_LIMIT_MAX) {
		size_t buffer_size = lrand48() % (remaining_size) + 1;

		/* if last IOV, allocate all remaining data in this last IOV */
		if (ctx->tx_count == UET_IOV_LIMIT_MAX - 1)
			buffer_size = remaining_size;

		remaining_size -= buffer_size;

		ctx->tx_iov[ctx->tx_count].iov_base =
			calloc(buffer_size, sizeof(char));

		if (ctx->tx_iov[ctx->tx_count].iov_base == NULL) {
			UET_PRINT_ERRNO("calloc");
			UET_ERR("Error allocating IO vector");
			goto exit;
		}

		ctx->tx_iov[ctx->tx_count].iov_len = buffer_size;


		if (!ctx->cfg.rma && !ctx->cfg.sync_rma &&
		    !ctx->cfg.atomic && !ctx->cfg.sync_atomic &&
		    ctx->cfg.client) {
			uet_init_iov_buf(
				buffer_size,
				ctx->tx_iov[ctx->tx_count].iov_base);
		}

		ctx->tx_count++;
	}

	/* Allocate IO Vectors for Receive */
	ctx->rx_iov = calloc(UET_IOV_LIMIT_MAX, sizeof(struct iovec));
	if (ctx->rx_iov == NULL) {
		UET_PRINT_ERRNO("calloc");
		UET_ERR("Error allocating IO vector");
		goto exit;
	}
	remaining_size = ctx->cfg.msg_size;
	while (remaining_size > 0 && ctx->rx_count < UET_IOV_LIMIT_MAX) {
		size_t buffer_size = lrand48() % (remaining_size) + 1;
		/* if last IOV, allocate all remaining data in this last IOV */
		if (ctx->rx_count == UET_IOV_LIMIT_MAX - 1)
			buffer_size = remaining_size;

		remaining_size -= buffer_size;

		ctx->rx_iov[ctx->rx_count].iov_base =
			calloc(buffer_size, sizeof(char));

		if (ctx->rx_iov[ctx->rx_count].iov_base == NULL) {
			UET_PRINT_ERRNO("calloc");
			UET_ERR("Error allocating IO vector");
			goto exit;
		}

		ctx->rx_iov[ctx->rx_count].iov_len = buffer_size;

		ctx->rx_count++;
	}

	rc = UET_SUCCESS_RC;

exit:
	if (hints)
		fi_freeinfo(hints);

	return rc;
}

/* wait for completion */
static uet_rc_t uet_compl_wait(uet_cq_handle_t cq_handle,
			       struct fi_cq_data_entry *cq_entry)
{
	ssize_t rc;
	struct fi_cq_err_entry err_entry;

	while (1) {
		rc = uet_cq_read(cq_handle, cq_entry, 1);
		if (rc < 0) {
			UET_ERR("uet_cq_read: %s", fi_strerror(-rc));
			if (rc == -FI_EAVAIL) {
				rc = uet_cq_readerr(cq_handle, &err_entry);
				if (rc < 0)
					UET_ERR("uet_cq_readerr: %s",
						fi_strerror(-rc));
				else if (rc == 0)
					UET_ERR("uet_cq_readerr: did "
						"not return entry");
				else
					UET_ERR("cq read err: %s",
						fi_strerror(
						       err_entry.err));
			}
			return UET_ERR_RC;
		} else if (rc == 1)
			break;
	}

	return UET_SUCCESS_RC;
}

/*
 * Bounded CQ poll: returns 1 if a completion was read, 0 on timeout, -1 on
 * error. Unlike uet_compl_wait() this never blocks forever. Needed for UUD,
 * where a lost datagram means a completion that will never arrive.
 */
static int uet_uud_poll(uet_cq_handle_t cq_handle,
			struct fi_cq_data_entry *cq_entry,
			int timeout_ms)
{
	struct fi_cq_err_entry err_entry;
	time_t start, now;
	ssize_t rc;

	uet_gettime(&start);
	while (1) {
		rc = uet_cq_read(cq_handle, cq_entry, 1);
		if (rc == 1)
			return 1;

		if (rc < 0) {
			if (rc == -FI_EAVAIL)
				uet_cq_readerr(cq_handle, &err_entry);

			UET_ERR("uet_cq_read (uud): %s", fi_strerror(-rc));

			return -1;
		}

		uet_gettime(&now);

		if ((now - start) >= timeout_ms)
			return 0;
	}
}

/*
 * perform client rma control message exchange as follows:
 *   - post rx buffer
 *   - send control message to server containing address and key for client's
 *     RMA buffer
 *   - wait for tx completion
 *   - wait for rx completion to receive message back from server containing
 *     address and key for server's RMA buffer
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_rma_client_ctrl_exchange(struct uet_context *ctx)
{
	int ret;
	void *context = NULL;
	struct uet_ctrl_msg *ctrl_msg;
	struct fi_cq_data_entry cq_entry;

	ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY, ctx->rx_msg,
		       ctx->cfg.msg_size, UET_NULL_HANDLE,
		       UET_NULL_HANDLE, context);
	if (ret != FI_SUCCESS) {
		UET_ERR("uet_recv: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	ctrl_msg = (struct uet_ctrl_msg *) ctx->tx_msg;
	*ctrl_msg = ctx->local_mr;

	ret = uet_send(ctx->ep_handle, ctx->cfg.job_id,
		       ctrl_msg, sizeof(struct uet_ctrl_msg),
		       UET_NULL_HANDLE, ctx->peer_addr_handle,
		       context);
	if (ret < 0) {
		UET_ERR("uet_send: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	ctrl_msg = (struct uet_ctrl_msg *) ctx->rx_msg;
	ctx->remote_mr = *ctrl_msg;

	return UET_SUCCESS_RC;
}

/*
 * perform server rma control message exchange as follows:
 *   - post rx buffer
 *   - wait for rx completion to receive message from client containing
 *     address and key for client's RMA buffer
 *   - send message back to client containing address and key for server's
 *     RMA buffer
 *   - wait for tx completion
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_rma_server_ctrl_exchange(struct uet_context *ctx)
{
	int ret;
	void *context = NULL;
	struct uet_ctrl_msg *ctrl_msg;
	struct fi_cq_data_entry cq_entry;

	ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY, ctx->rx_msg,
		       ctx->cfg.msg_size, UET_NULL_HANDLE,
		       UET_NULL_HANDLE, context);
	if (ret != FI_SUCCESS) {
		UET_ERR("uet_recv: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	ctrl_msg = (struct uet_ctrl_msg *) ctx->rx_msg;
	ctx->remote_mr = *ctrl_msg;

	ctrl_msg = (struct uet_ctrl_msg *) ctx->tx_msg;
	*ctrl_msg = ctx->local_mr;
	ret = uet_send(ctx->ep_handle, ctx->cfg.job_id,
		       ctrl_msg, sizeof(struct uet_ctrl_msg),
		       UET_NULL_HANDLE, ctx->peer_addr_handle,
		       context);
	if (ret < 0) {
		UET_ERR("uet_send: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * rma write / read, using the segment form of the api when the local buffer
 * has been described as segments. The two forms move identical bytes; only
 * how the local buffer is named differs.
 */

static ssize_t uet_test_write(struct uet_context *ctx, uint64_t *imm_data,
			      size_t len, void *context)
{
	if ((ctx->local_seg != NULL) && (len == ctx->cfg.msg_size))
		return uet_writeseg(ctx->ep_handle, ctx->cfg.job_id,
				    ctx->local_seg, ctx->local_seg_count,
				    imm_data, ctx->peer_addr_handle,
				    ctx->remote_mr.rma_buf_addr,
				    ctx->remote_mr.key, context);

	return uet_write(ctx->ep_handle, ctx->cfg.job_id, ctx->mr_buf, len,
			 imm_data, ctx->mr_handle, ctx->peer_addr_handle,
			 ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			 context);
}

static ssize_t uet_test_read(struct uet_context *ctx, size_t len,
			     void *context)
{
	if ((ctx->local_seg != NULL) && (len == ctx->cfg.msg_size))
		return uet_readseg(ctx->ep_handle, ctx->cfg.job_id,
				   ctx->local_seg, ctx->local_seg_count,
				   ctx->peer_addr_handle,
				   ctx->remote_mr.rma_buf_addr,
				   ctx->remote_mr.key, context);

	return uet_read(ctx->ep_handle, ctx->cfg.job_id, ctx->mr_buf, len,
			ctx->mr_handle, ctx->peer_addr_handle,
			ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			context);
}

static uet_rc_t uet_rma_write_data(struct uet_context *ctx, uint64_t *imm_data)
{
	struct fi_cq_data_entry cq_entry;
	ssize_t ret;

	/* For RUDI the bulk data is written over RUDI. Once that completes,
	 * a separate zero-length WRITE-with-immediate over RUD delivers
	 * the target completion. Otherwise a single write-with-immediate
	 * carries both data and immediate.
	 */
	if (ctx->cfg.rudi) {
		/* bulk data over RUDI (no immediate) */
		ret = uet_test_write(ctx, NULL, ctx->cfg.msg_size, NULL);
		if (ret < 0) {
			UET_ERR("uet_write (RUDI data): %s", fi_strerror(-ret));
			return UET_ERR_RC;
		}

		if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) !=
		    UET_SUCCESS_RC) {
			UET_ERR("uet_compl_wait (RUDI data)");
			return UET_ERR_RC;
		}

		/* signal over RUD (zero-length write-with-immediate) */
		ret = uet_test_write(ctx, imm_data, 0, NULL);
		if (ret < 0) {
			UET_ERR("uet_write (RUD imm): %s", fi_strerror(-ret));
			return UET_ERR_RC;
		}

		if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) !=
		    UET_SUCCESS_RC) {
			UET_ERR("uet_compl_wait (RUD imm)");
			return UET_ERR_RC;
		}

		return UET_SUCCESS_RC;
	}

	/* single write-with-immediate (data + target completion) */
	ret = uet_test_write(ctx, imm_data, ctx->cfg.msg_size, NULL);
	if (ret < 0) {
		UET_ERR("uet_write: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform client RMA data transfer exchange as follows:
 *   - read data from server RMA buffer
 *   - wait for read completion
 *   - write to server RMA buffer
 *     - include immediate data to generate completion at server
 *   - wait for write completion
 *   - wait for remote write completion to indicate data has been written
 *     back to client's RMA buffer by server
 *   - validate data is correct
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_rma_client(struct uet_context *ctx)
{
	ssize_t ret;
	void *context = NULL;
	uint64_t imm_data = UET_WRITE_IMM_DATA;
	struct fi_cq_data_entry cq_entry;

	ret = uet_test_read(ctx, ctx->cfg.msg_size, context);
	if (ret < 0) {
		UET_ERR("uet_read: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (uet_rma_write_data(ctx, &imm_data) != UET_SUCCESS_RC)
		return UET_ERR_RC;

	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (uet_validate_msg(ctx, ctx->mr_buf) != UET_SUCCESS_RC) {
		UET_ERR("Invalid buffer contents");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform server RMA data transfer exchange as follows:
 *   - wait for remote write completion to indicate data has been written to
 *     the server RMA buffer by the client
 *   - validate data is correct
 *   - write data back to client RMA buffer
 *     - include immediate data to generate completion at client
 *   - wait for write completion
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_rma_server(struct uet_context *ctx)
{
	ssize_t ret;
	void *context = NULL;
	uint64_t imm_data = UET_WRITE_IMM_DATA;
	struct fi_cq_data_entry cq_entry;

	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (uet_validate_msg(ctx, ctx->mr_buf) != UET_SUCCESS_RC) {
		UET_ERR("Invalid buffer contents");
		return UET_ERR_RC;
	}

	if (uet_rma_write_data(ctx, &imm_data) != UET_SUCCESS_RC)
		return UET_ERR_RC;

	return UET_SUCCESS_RC;
}

/*
 * perform client sync group RMA data transfer exchange as follows:
 *   - read data from server RMA buffer
 *   - wait for read completion
 *   - write to server RMA buffer
 *     - write is series of sync write operations followed by sync write
 *       with immediate data to generate completion at server
 *   - wait for write completion
 *   - wait for remote write completion to indicate data has been written
 *     back to client's RMA buffer by server
 *   - validate data is correct
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_sync_rma_client(struct uet_context *ctx)
{
	ssize_t ret;
	void *context = NULL;
	uint64_t imm_data = UET_WRITE_IMM_DATA;
	size_t msg1_sz, msg2_sz, msg3_sz;
	struct fi_cq_data_entry cq_entry;

	ret = uet_test_read(ctx, ctx->cfg.msg_size, context);
	if (ret < 0) {
		UET_ERR("uet_read: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	msg1_sz = ctx->cfg.msg_size / 3;
	msg2_sz = msg1_sz;
	msg3_sz = (ctx->cfg.msg_size - msg1_sz) - msg2_sz;

	ret = uet_write_sync(ctx->ep_handle, ctx->cfg.job_id, ctx->mr_buf,
			     msg1_sz, NULL, ctx->mr_handle,
			     ctx->peer_addr_handle, ctx->remote_mr.rma_buf_addr,
			     ctx->remote_mr.key, context);
	if (ret < 0) {
		UET_ERR("uet_write sync: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	ret = uet_write_sync(ctx->ep_handle, ctx->cfg.job_id,
			     ctx->mr_buf + msg1_sz,
			     msg2_sz, NULL, ctx->mr_handle,
			     ctx->peer_addr_handle,
			     ctx->remote_mr.rma_buf_addr + msg1_sz,
			     ctx->remote_mr.key, context);
	if (ret < 0) {
		UET_ERR("uet_write_sync: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	ret = uet_write_sync(ctx->ep_handle, ctx->cfg.job_id,
			     ctx->mr_buf + msg1_sz + msg2_sz,
			     msg3_sz, &imm_data, ctx->mr_handle,
			     ctx->peer_addr_handle,
			     ctx->remote_mr.rma_buf_addr + msg1_sz + msg2_sz,
			     ctx->remote_mr.key, context);
	if (ret < 0) {
		UET_ERR("uet_write_sync: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (uet_validate_msg(ctx, ctx->mr_buf) != UET_SUCCESS_RC) {
		UET_ERR("Invalid buffer contents");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform server sync RMA data transfer exchange
 */
static uet_rc_t uet_sync_rma_server(struct uet_context *ctx)
{
	return uet_rma_server(ctx);
}

#define ATOMIC_SUM_VALUE	2

/*
 * perform client atomic data transfer exchange as follows:
 *   - query support for atomic operation that is expected to be unsupported
 *   - query support for atomic operations that are expected to be supported
 *   - do atomic fetch add of 0 to get current value from server RMA buffer
 *   - wait for atomic fetch completion
 *   - do atomic non-fetch add of known constant to server RMA buffer
 *   - wait for atomic completion
 *   - validate result
 *   - do atomic fetch add of known constant to server RMA buffer
 *   - wait for atomic fetch completion
 *   - validate result
 *   - do atomic fetch add of known constant to server RMA buffer
 *   - validate result
 *   - do atomic cswap using value that is expected to fail compare
 *   - wait for atomic cswap completion
 *   - validate result
 *   - do atomic cswap using value that is expected to swap
 *   - wait for atomic cswap completion
 *   - validate result
 *   - write to server RMA buffer
 *     - include immediate data to generate completion at server
 *   - wait for write completion
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_atomic_client(struct uet_context *ctx)
{
	int query_ret;
	ssize_t ret;
	struct fi_atomic_attr attr;
	uint64_t *local_op_buf, *result_buf, *compare_buf, val;
	void *context = NULL;
	uint64_t imm_data = UET_WRITE_IMM_DATA;
	struct fi_cq_data_entry cq_entry;

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT32, FI_SUM,
				     &attr, FI_FETCH_ATOMIC);
	if (query_ret != -FI_EOPNOTSUPP) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT64, FI_SUM,
				     &attr, FI_ATOMIC);
	if (query_ret < 0) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	if ((attr.count != 1) || (attr.size != sizeof(uint64_t))) {
		UET_ERR("uet_query_atomic: bad attr");
		return UET_ERR_RC;
	}

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT64, FI_SUM,
				     &attr, FI_FETCH_ATOMIC);
	if (query_ret < 0) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	if ((attr.count != 1) || (attr.size != sizeof(uint64_t))) {
		UET_ERR("uet_query_atomic: bad attr");
		return UET_ERR_RC;
	}

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT64, FI_CSWAP,
				     &attr, FI_COMPARE_ATOMIC);
	if (query_ret != FI_SUCCESS) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	if ((attr.count != 1) || (attr.size != sizeof(uint64_t))) {
		UET_ERR("uet_query_atomic: bad attr");
		return UET_ERR_RC;
	}

	if (ctx->cfg.msg_size < UET_MIN_ATOMIC_MSG_SIZE) {
		UET_ERR("atomic test requires min msg size of %d",
	                UET_MIN_ATOMIC_MSG_SIZE);
		return UET_ERR_RC;
	}

	local_op_buf = (uint64_t *) ctx->mr_buf;
	result_buf = local_op_buf + 1;
	compare_buf = result_buf + 1;

	*local_op_buf = 0;

	ret = uet_fetch_atomic(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
			       attr.count, ctx->mr_handle, result_buf,
			       ctx->mr_handle, ctx->peer_addr_handle,
			       ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			       FI_UINT64, FI_SUM, context);
	if (ret < 0) {
		UET_ERR("uet_fetch_atomic: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	val = *result_buf;

	*local_op_buf = (uint64_t) ATOMIC_SUM_VALUE;

	ret = uet_atomic(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
		         attr.count, ctx->mr_handle, ctx->peer_addr_handle,
			 ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			 FI_UINT64, FI_SUM, context);
	if (ret < 0) {
		UET_ERR("uet_atomic: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	val += *local_op_buf;

	ret = uet_fetch_atomic(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
			       attr.count, ctx->mr_handle, result_buf,
			       ctx->mr_handle, ctx->peer_addr_handle,
			       ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			       FI_UINT64, FI_SUM, context);
	if (ret < 0) {
		UET_ERR("uet_fetch_atomic: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (*result_buf != val) {
		UET_ERR("uet_fetch_atomic: bad result %lu", *result_buf);
		return UET_ERR_RC;
	}

	ret = uet_fetch_atomic(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
			       attr.count, ctx->mr_handle, result_buf,
			       ctx->mr_handle, ctx->peer_addr_handle,
			       ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			       FI_UINT64, FI_SUM, context);
	if (ret < 0) {
		UET_ERR("uet_fetch_atomic: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	val += *local_op_buf;

	if (*result_buf != val) {
		UET_ERR("uet_fetch_atomic: bad result %lu", *result_buf);
		return UET_ERR_RC;
	}

	val += *local_op_buf;
	*local_op_buf = val + 1;
	*compare_buf = val - 1;

	ret = uet_compare_atomic(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
				 attr.count, ctx->mr_handle, compare_buf,
				 ctx->mr_handle, result_buf, ctx->mr_handle,
				 ctx->peer_addr_handle,
				 ctx->remote_mr.rma_buf_addr,
				 ctx->remote_mr.key,
				 FI_UINT64, FI_CSWAP, context);
	if (ret < 0) {
		UET_ERR("uet_compare_atomic: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (*result_buf != val) {
		UET_ERR("uet_compare_atomic: bad result %lu", *result_buf);
		return UET_ERR_RC;
	}

	*compare_buf = val;

	ret = uet_compare_atomic(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
				 attr.count, ctx->mr_handle, compare_buf,
				 ctx->mr_handle, result_buf, ctx->mr_handle,
				 ctx->peer_addr_handle,
				 ctx->remote_mr.rma_buf_addr,
				 ctx->remote_mr.key,
				 FI_UINT64, FI_CSWAP, context);
	if (ret < 0) {
		UET_ERR("uet_compare_atomic: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (*result_buf != val) {
		UET_ERR("uet_compare_atomic: bad result %lu", *result_buf);
		return UET_ERR_RC;
	}

	ret = uet_test_write(ctx, &imm_data, ctx->cfg.msg_size, context);
	if (ret < 0) {
		UET_ERR("uet_write: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform server atomic data transfer exchange as follows:
 *   - wait for remote write completion to indicate client has completed atomic
 *     operation tests
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_atomic_server(struct uet_context *ctx)
{
	struct fi_cq_data_entry cq_entry;

	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform client sync atomic data transfer exchange as follows:
 *   - query support for atomic operation that is expected to be unsupported
 *   - query support for atomic operations that are expected to be supported
 *   - do sync write of known constant to server RMA buffer
 *   - wait for write completion
 *   - do sync atomic add of known constant to server RMA buffer
 *   - wait for sync atomic completion
 *   - do atomic fetch add of 0 to server RMA buffer
 *   - wait for atomic fetch completion
 *   - validate result
 *   - do 0-byte write to server RMA buffer
 *     - include immediate data to generate completion at server
 *   - wait for write completion
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_sync_atomic_client(struct uet_context *ctx)
{
	int query_ret;
	ssize_t ret;
	struct fi_atomic_attr attr;
	uint64_t *local_op_buf, *result_buf, val;
	void *context = NULL;
	uint64_t imm_data = UET_WRITE_IMM_DATA;
	struct fi_cq_data_entry cq_entry;

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT32, FI_SUM,
				     &attr, FI_FETCH_ATOMIC);
	if (query_ret != -FI_EOPNOTSUPP) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT64, FI_SUM,
				     &attr, FI_ATOMIC);
	if (query_ret < 0) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	if ((attr.count != 1) || (attr.size != sizeof(uint64_t))) {
		UET_ERR("uet_query_atomic: bad attr");
		return UET_ERR_RC;
	}

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT64, FI_SUM,
				     &attr, FI_FETCH_ATOMIC);
	if (query_ret < 0) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	if ((attr.count != 1) || (attr.size != sizeof(uint64_t))) {
		UET_ERR("uet_query_atomic: bad attr");
		return UET_ERR_RC;
	}

	query_ret = uet_query_atomic(ctx->domain_handle, FI_UINT64, FI_CSWAP,
				     &attr, FI_COMPARE_ATOMIC);
	if (query_ret != FI_SUCCESS) {
		UET_ERR("uet_query_atomic: %s", fi_strerror(-query_ret));
		return UET_ERR_RC;
	}

	if ((attr.count != 1) || (attr.size != sizeof(uint64_t))) {
		UET_ERR("uet_query_atomic: bad attr");
		return UET_ERR_RC;
	}

	if (ctx->cfg.msg_size < UET_MIN_ATOMIC_MSG_SIZE) {
		UET_ERR("atomic test requires min msg size of %d",
	                UET_MIN_ATOMIC_MSG_SIZE);
		return UET_ERR_RC;
	}

	local_op_buf = (uint64_t *) ctx->mr_buf;
	*local_op_buf = (uint64_t) ATOMIC_SUM_VALUE;
	val = *local_op_buf;

	ret = uet_write_sync(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
			     sizeof(uint64_t), NULL, ctx->mr_handle,
			     ctx->peer_addr_handle, ctx->remote_mr.rma_buf_addr,
			     ctx->remote_mr.key, context);
	if (ret < 0) {
		UET_ERR("uet_write_sync: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	ret = uet_atomic_sync(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
		              attr.count, ctx->mr_handle, ctx->peer_addr_handle,
			      ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			      FI_UINT64, FI_SUM, context);
	if (ret < 0) {
		UET_ERR("uet_atomic_sync: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	val += *local_op_buf;
	*local_op_buf = 0;
	result_buf = local_op_buf + 1;

	ret = uet_fetch_atomic(ctx->ep_handle, ctx->cfg.job_id, local_op_buf,
			       attr.count, ctx->mr_handle, result_buf,
			       ctx->mr_handle, ctx->peer_addr_handle,
			       ctx->remote_mr.rma_buf_addr, ctx->remote_mr.key,
			       FI_UINT64, FI_SUM, context);
	if (ret < 0) {
		UET_ERR("uet_fetch_atomic: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (*result_buf != val) {
		UET_ERR("uet_fetch_atomic: bad result %lu", *result_buf);
		return UET_ERR_RC;
	}

	ret = uet_write_sync(ctx->ep_handle, ctx->cfg.job_id, ctx->mr_buf,
			     0, &imm_data, ctx->mr_handle,
			     ctx->peer_addr_handle, ctx->remote_mr.rma_buf_addr,
			     ctx->remote_mr.key, context);
	if (ret < 0) {
		UET_ERR("uet_write_sync: %s", fi_strerror(-ret));
		return UET_ERR_RC;
	}

	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform server sync atomic data transfer exchange as follows:
 *   - wait for remote write completion to client has completed atomic
 *     operation tests
 *
 * returns:
 *   UET_SUCCESS_RC
 *   UET_ERROR_RC
 */
static uet_rc_t uet_sync_atomic_server(struct uet_context *ctx)
{
	struct fi_cq_data_entry cq_entry;

	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform client message data transfer for unexpected message test
 *   - send message to server
 *   - wait for message to be echoed back
 */
static uet_rc_t uet_msg_client_unexpected(struct uet_context *ctx)
{
	ssize_t ret;
	time_t start, now, delta;
	struct fi_cq_data_entry cq_entry;
	uet_addr_handle_t addr_handle;

	if (ctx->cfg.iov_test) {
		if (ctx->cfg.tag) {
			if (ctx->cfg.tag_any_src)
				addr_handle = UET_NULL_HANDLE;
			else
				addr_handle = ctx->peer_addr_handle;

			if (!first) {
				/* post tagged rx buffer */
				ret = uet_trecvv(ctx->ep_handle, UET_JOB_ID_ANY,
						ctx->rx_iov, ctx->rx_count,
						UET_NULL_HANDLE, addr_handle,
						UET_DEFAULT_TAG, UET_EXACT_MATCH, NULL);
				if (ret < 0) {
					UET_ERR("uet_trecvv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			}
			ret = uet_tsendv(ctx->ep_handle, ctx->cfg.job_id,
					ctx->tx_iov, ctx->tx_count,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					UET_DEFAULT_TAG, NULL);
			if (ret < 0) {
				UET_ERR("uet_tsendv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			if (!first) {
				/* post rx buffer */
				ret = uet_recvv(ctx->ep_handle, UET_JOB_ID_ANY,
						ctx->rx_iov, ctx->rx_count,
						UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
				if (ret < 0) {
					UET_ERR("uet_recvv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			}
			ret = uet_sendv(ctx->ep_handle, ctx->cfg.job_id,
					ctx->tx_iov, ctx->tx_count,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					NULL);
			if (ret < 0) {
				UET_ERR("uet_sendv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		}
	} else {
		if (ctx->cfg.tag) {
			if (ctx->cfg.tag_any_src)
				addr_handle = UET_NULL_HANDLE;
			else
				addr_handle = ctx->peer_addr_handle;
			if (!first) {
				ret = uet_trecv(ctx->ep_handle, UET_JOB_ID_ANY,
						ctx->rx_msg, ctx->cfg.msg_size,
						UET_NULL_HANDLE, addr_handle,
						UET_DEFAULT_TAG, UET_EXACT_MATCH, NULL);
				if (ret < 0) {
					UET_ERR("uet_trecv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			}
			ret = uet_tsend(ctx->ep_handle, ctx->cfg.job_id,
						ctx->tx_msg, ctx->cfg.msg_size,
						UET_NULL_HANDLE, ctx->peer_addr_handle,
						UET_DEFAULT_TAG, NULL);
			if (ret < 0) {
				UET_ERR("uet_send: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			if (!first) {
				ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY,
							ctx->rx_msg, ctx->cfg.msg_size,
							UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
				if (ret < 0) {
					UET_ERR("uet_recv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			}
			ret = uet_send(ctx->ep_handle, ctx->cfg.job_id,
						ctx->tx_msg, ctx->cfg.msg_size,
						UET_NULL_HANDLE, ctx->peer_addr_handle,
						NULL);
				if (ret < 0) {
					UET_ERR("uet_send: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
		}
	}

	/* wait for tx completion */
	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	if (first) {
		/*
		 * - delay to give time for unexpected message protocol to complete
		 *     - server echoes messages back
		 *     - client responds with RC_NO_MATCH since there is no rx buffer posted
		 *     - server sends UET_MSG_ERROR and starts message retransmisson timer
		 * - then post buffer so that message retransmission succeeds
		 * - the minimum message retransmission delay is 50 msecs
		 */
		uet_gettime(&start);
#define UNEXPECTED_MSG_TEST_DELAY 3 /* msecs */
		for (now = start, delta = 0;
		     delta < UNEXPECTED_MSG_TEST_DELAY;
		     delta = now - start) {
			uet_cq_read(ctx->tx_cq_handle, &cq_entry, 0); /* enable forward progress */
			uet_gettime(&now);
		}
		first = false;

		if (ctx->cfg.iov_test) {
			if (ctx->cfg.tag) {
				/* post tagged rx buffer */
				ret = uet_trecvv(ctx->ep_handle, UET_JOB_ID_ANY,
						ctx->rx_iov, ctx->rx_count,
						UET_NULL_HANDLE, addr_handle,
						UET_DEFAULT_TAG, UET_EXACT_MATCH, NULL);
				if (ret < 0) {
					UET_ERR("uet_trecvv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			} else {
				/* post rx buffer */
				ret = uet_recvv(ctx->ep_handle, UET_JOB_ID_ANY,
						ctx->rx_iov, ctx->rx_count,
						UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
				if (ret < 0) {
					UET_ERR("uet_recvv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			}
		} else {
			if (ctx->cfg.tag) {
				/* post tagged rx buffer */
				ret = uet_trecv(ctx->ep_handle, UET_JOB_ID_ANY,
						ctx->rx_msg, ctx->cfg.msg_size,
						UET_NULL_HANDLE, addr_handle,
						UET_DEFAULT_TAG, UET_EXACT_MATCH, NULL);
				if (ret < 0) {
					UET_ERR("uet_trecv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			} else {
				ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY,
				       ctx->rx_msg, ctx->cfg.msg_size,
				       UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
				if (ret < 0) {
					UET_ERR("uet_recv: %s", fi_strerror(-ret));
					return UET_ERR_RC;
				}
			}
		}
	}

	/* wait for rx completion */
	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	/* Message validation for IOV*/
	if (ctx->cfg.iov_test) {
		if (uet_validate_iov_msg(ctx) != UET_SUCCESS_RC) {
			UET_ERR("Invalid iov data in RX-iov");
			return UET_ERR_RC;
		}
	}

	if (cq_entry.len != ctx->cfg.msg_size) {
		UET_ERR("Received bad message size = %lu", cq_entry.len);
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * UUD datagram test (best-effort, single-packet). A flow-controlled lock-step
 * ping-pong: the client sends one untagged datagram and waits (bounded) for
 * the server to echo it, before sending the next. Keeping a single datagram
 * in flight means the server always has a receive posted (no unexpected
 * message overrun) and paces the sender without any ACK/RTO. On a clean
 * network every datagram round-trips. Under impairment a datagram may be
 * LOST with no recovery. The bounded wait times out, the loss is counted, and
 * both sides re-sync on the next datagram.
 */
static uet_rc_t uet_uud_client(struct uet_context *ctx)
{
	struct fi_cq_data_entry cq_entry;
	bool impaired = false;
	bool rx_posted = false;
	int ok = 0, lost = 0, i;
	ssize_t ret;
	int got;

	impaired = (getenv("UET_IMPAIRMENT_SHIM") != NULL);

	for (i = 0; i < UET_UUD_BLAST_N; i++) {
		/* One receive outstanding for the echo. Reposted only after
		 * it is consumed so a lost echo leaves it to catch the next
		 * one.
		 */
		if (!rx_posted) {
			ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY,
				       ctx->rx_msg, ctx->cfg.msg_size,
				       UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
			if (ret < 0) {
				UET_ERR("uet_recv (uud): %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}

			rx_posted = true;
		}

		ret = uet_send(ctx->ep_handle, ctx->cfg.job_id, ctx->tx_msg,
			       ctx->cfg.msg_size, UET_NULL_HANDLE,
			       ctx->peer_addr_handle, NULL);
		if (ret < 0) {
			UET_ERR("uet_send (uud): %s", fi_strerror(-ret));
			return UET_ERR_RC;
		}

		/* UUD tx completes immediately (fire and forget) */
		if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) !=
		    UET_SUCCESS_RC) {
			UET_ERR("uet_compl_wait (uud tx)");
			return UET_ERR_RC;
		}

		/* Bounded wait for the echo. If missed then this datagram
		 * was lost with no recovery.
		 */
		got = uet_uud_poll(ctx->rx_cq_handle, &cq_entry,
				   UET_UUD_RX_TIMEOUT_MS);
		if (got < 0)
			return UET_ERR_RC;

		if (got == 1) {
			ok++;
			rx_posted = false;   /* echo consumed the recv */
		} else {
			lost++;              /* recv stays posted; re-syncs */
		}
	}

	printf("UUD client: %d/%d datagrams round-tripped (%d lost)\n",
	       ok, UET_UUD_BLAST_N, lost);

	/* On a clean network every datagram MUST round-trip. Under impairment,
	 * loss is expected (fire-and-forget, no recovery) and is best effort.
	 */
	if (lost && !impaired) {
		UET_ERR("UUD clean: %d datagrams lost with no impairment", lost);
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

static uet_rc_t uet_uud_server(struct uet_context *ctx)
{
	struct fi_cq_data_entry cq_entry;
	bool rx_posted = false;
	int ok = 0, quiet = 0, tmo;
	ssize_t ret;
	int got;

	/* Drain until all datagrams are received or the sender goes quiet.
	 * Uses two-phase timing. Wait a long time for the FIRST datagram
	 * then short quiet windows to detect the end of the blast.
	 */
	while ((ok < UET_UUD_BLAST_N) && (quiet < UET_UUD_DRAIN_QUIET)) {
		if (!rx_posted) {
			ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY,
				       ctx->rx_msg, ctx->cfg.msg_size,
				       UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
			if (ret < 0) {
				UET_ERR("uet_recv (uud): %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}

			rx_posted = true;
		}

		tmo = (ok == 0) ? UET_UUD_FIRST_TIMEOUT_MS
				: UET_UUD_RX_TIMEOUT_MS;

		got = uet_uud_poll(ctx->rx_cq_handle, &cq_entry, tmo);
		if (got < 0)
			return UET_ERR_RC;

		if (got == 0) {
			quiet++;             /* recv stays posted */
			continue;
		}

		quiet = 0;
		rx_posted = false;

		if (cq_entry.len != ctx->cfg.msg_size) {
			UET_ERR("UUD: bad datagram size = %lu", cq_entry.len);
			return UET_ERR_RC;
		}

		if (uet_validate_msg(ctx, ctx->rx_msg) != UET_SUCCESS_RC) {
			UET_ERR("UUD: bad datagram data");
			return UET_ERR_RC;
		}

		ok++;

		/* echo the datagram back over UUD (fire and forget) */
		ret = uet_send(ctx->ep_handle, ctx->cfg.job_id, ctx->tx_msg,
			       ctx->cfg.msg_size, UET_NULL_HANDLE,
			       ctx->peer_addr_handle, NULL);
		if (ret < 0) {
			UET_ERR("uet_send (uud echo): %s", fi_strerror(-ret));
			return UET_ERR_RC;
		}

		if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) !=
		    UET_SUCCESS_RC) {
			UET_ERR("uet_compl_wait (uud echo tx)");
			return UET_ERR_RC;
		}
	}

	printf("UUD server: %d/%d datagrams received and echoed\n",
	       ok, UET_UUD_BLAST_N);

	return UET_SUCCESS_RC;
}

/*
 * perform client message data transfer as follows:
 *   - send message to server
 *   - wait for message to be echoed back
 */
static uet_rc_t uet_msg_client(struct uet_context *ctx)
{
	ssize_t ret;
	struct fi_cq_data_entry cq_entry;
	uet_addr_handle_t addr_handle;

	if (ctx->cfg.iov_test) {
		if (ctx->cfg.tag) {
			if (ctx->cfg.tag_any_src)
				addr_handle = UET_NULL_HANDLE;
			else
				addr_handle = ctx->peer_addr_handle;

			/* post tagged rx buffer */
			ret = uet_trecvv(ctx->ep_handle, UET_JOB_ID_ANY,
					ctx->rx_iov, ctx->rx_count,
					UET_NULL_HANDLE, addr_handle,
					UET_DEFAULT_TAG, UET_EXACT_MATCH, NULL);
			if (ret < 0) {
				UET_ERR("uet_trecvv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}

			/* transmit tagged message to server */
			ret = uet_tsendv(ctx->ep_handle, ctx->cfg.job_id,
					ctx->tx_iov, ctx->tx_count,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					UET_DEFAULT_TAG, NULL);
			if (ret < 0) {
				UET_ERR("uet_tsendv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			/* post rx buffer */
			ret = uet_recvv(ctx->ep_handle, UET_JOB_ID_ANY,
					ctx->rx_iov, ctx->rx_count,
					UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
			if (ret < 0) {
				UET_ERR("uet_recvv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}

			/* transmit message to server */
			ret = uet_sendv(ctx->ep_handle, ctx->cfg.job_id,
					ctx->tx_iov, ctx->tx_count,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					NULL);
			if (ret < 0) {
				UET_ERR("uet_sendv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		}
	} else {
		if (ctx->cfg.tag) {
			if (ctx->cfg.tag_any_src)
				addr_handle = UET_NULL_HANDLE;
			else
				addr_handle = ctx->peer_addr_handle;

			/* post tagged rx buffer */
			ret = uet_trecv(ctx->ep_handle, UET_JOB_ID_ANY, ctx->rx_msg,
					ctx->cfg.msg_size, UET_NULL_HANDLE,
					addr_handle, UET_DEFAULT_TAG,
					UET_EXACT_MATCH, NULL);
			if (ret < 0) {
				UET_ERR("uet_trecv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}

			/* transmit tagged message to server */
			ret = uet_tsend(ctx->ep_handle, ctx->cfg.job_id,
					ctx->tx_msg, ctx->cfg.msg_size,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					UET_DEFAULT_TAG, NULL);
			if (ret < 0) {
				UET_ERR("uet_tsend: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			/* post rx buffer */
			ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY, ctx->rx_msg,
					ctx->cfg.msg_size, UET_NULL_HANDLE,
					UET_NULL_HANDLE, NULL);
			if (ret < 0) {
				UET_ERR("uet_recv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}

			/* transmit message to server */
			ret = uet_send(ctx->ep_handle, ctx->cfg.job_id,
					ctx->tx_msg, ctx->cfg.msg_size,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					NULL);
			if (ret < 0) {
				UET_ERR("uet_send: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		}
	}

	/* wait for tx completion */
	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	/* wait for rx completion */
	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	/* Message validation for IOV*/
	if (ctx->cfg.iov_test) {
		if (uet_validate_iov_msg(ctx) != UET_SUCCESS_RC) {
			UET_ERR("Invalid iov data in RX-iov");
			return UET_ERR_RC;
		}
	}

	if (cq_entry.len != ctx->cfg.msg_size) {
		UET_ERR("Received bad message size = %lu", cq_entry.len);
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/*
 * perform server message data transfer as follows:
 *   - wait for message from client
 *   - validate data is correct
 *   - echo message back to server
 */
static uet_rc_t uet_msg_server(struct uet_context *ctx)
{
	ssize_t ret;
	struct fi_cq_data_entry cq_entry;
	uet_addr_handle_t addr_handle;

	if (ctx->cfg.iov_test) {
		if (ctx->cfg.tag) {
			if (ctx->cfg.tag_any_src)
				addr_handle = UET_NULL_HANDLE;
			else
				addr_handle = ctx->peer_addr_handle;

			/* post tagged rx buffer */
			ret = uet_trecvv(ctx->ep_handle, UET_JOB_ID_ANY,
					ctx->rx_iov, ctx->rx_count,
					UET_NULL_HANDLE, addr_handle,
					UET_DEFAULT_TAG, UET_EXACT_MATCH, NULL);
			if (ret < 0) {
				UET_ERR("uet_trecvv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			/* post rx buffer */
			ret = uet_recvv(ctx->ep_handle, UET_JOB_ID_ANY,
					ctx->rx_iov, ctx->rx_count,
					UET_NULL_HANDLE, UET_NULL_HANDLE, NULL);
			if (ret < 0) {
				UET_ERR("uet_recvv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		}
	} else {
		if (ctx->cfg.tag) {
			if (ctx->cfg.tag_any_src)
				addr_handle = UET_NULL_HANDLE;
			else
				addr_handle = ctx->peer_addr_handle;

			/* post tagged rx buffer */
			ret = uet_trecv(ctx->ep_handle, UET_JOB_ID_ANY, ctx->rx_msg,
					ctx->cfg.msg_size, UET_NULL_HANDLE,
					addr_handle, UET_DEFAULT_TAG,
					UET_EXACT_MATCH, NULL);
			if (ret < 0) {
				UET_ERR("uet_trecv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			/* post rx buffer */
			ret = uet_recv(ctx->ep_handle, UET_JOB_ID_ANY, ctx->rx_msg,
					ctx->cfg.msg_size, UET_NULL_HANDLE,
					UET_NULL_HANDLE, NULL);
			if (ret < 0) {
				UET_ERR("uet_recv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		}
	}

	/* wait for rx completion */
	if (uet_compl_wait(ctx->rx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	/* validate buffer contents */
	/* IOV message validation will be in the CLIENT side*/
	if (!ctx->cfg.iov_test) {
		if (uet_validate_msg(ctx, ctx->rx_msg) != UET_SUCCESS_RC) {
			UET_ERR("Invalid buffer contents");
			return UET_ERR_RC;
		}
	}

	if (ctx->cfg.iov_test) {
		if (ctx->cfg.tag) {
			/* echo tagged message back to client */
			ret = uet_tsendv(ctx->ep_handle, ctx->cfg.job_id,
					ctx->rx_iov, ctx->rx_count,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					UET_DEFAULT_TAG, NULL);
			if (ret < 0) {
				UET_ERR("uet_tsendv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			/* echo message back to client */
			ret = uet_sendv(ctx->ep_handle, ctx->cfg.job_id,
					ctx->rx_iov, ctx->rx_count,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					NULL);
			if (ret < 0) {
				UET_ERR("uet_sendv: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		}
	} else {
		if (ctx->cfg.tag) {
			/* echo tagged message back to client */
			ret = uet_tsend(ctx->ep_handle, ctx->cfg.job_id,
					ctx->rx_msg, cq_entry.len,
					UET_NULL_HANDLE, ctx->peer_addr_handle,
					UET_DEFAULT_TAG, NULL);
			if (ret < 0) {
				UET_ERR("uet_tsend: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		} else {
			/* echo message back to client */
			ret = uet_send(ctx->ep_handle, ctx->cfg.job_id,
					ctx->rx_msg, cq_entry.len, UET_NULL_HANDLE,
					ctx->peer_addr_handle, NULL);
			if (ret < 0) {
				UET_ERR("uet_send: %s", fi_strerror(-ret));
				return UET_ERR_RC;
			}
		}
	}

	/* wait for tx completion */
	if (uet_compl_wait(ctx->tx_cq_handle, &cq_entry) != UET_SUCCESS_RC) {
		UET_ERR("uet_compl_wait");
		return UET_ERR_RC;
	}

	return UET_SUCCESS_RC;
}

/* do one run */
static int uet_run(int argc, char *argv[], struct uet_context *ctx)
{
	uet_rc_t rc;
	int iteration = 0;
	int use_iov = 0;
	char *pds;

	/* init config parms */
	rc = uet_init_cfg(argc, argv, ctx);
	if (rc != UET_SUCCESS_RC) {
		UET_USAGE(argv[0]);
		goto exit;
	}

	/* init transport */
	rc = uet_init_transport(ctx);
	if (rc != UET_SUCCESS_RC)
		goto exit;

	/*
	 * Give the server time to finish NIC initialization before the client
	 * starts sending. i.e., XDP initialization (BPF program load, attach,
	 * UMEM setup, socket creation, xsks_map config) takes significantly
	 * longer than rawsock. Without this delay the client can blast
	 * packets before the server's AF_XDP path is fully ready, causing
	 * missed packets and retransmits.
	 */
	if (ctx->cfg.client)
		sleep(1);

	for (use_iov = 0; use_iov <= 1; use_iov++) {
		pds = getenv(UET_PDS);
		/* TODO: IOV support for SNG mode.
		 * TODO: IOV support for UUD mode.
		 */
		if (((pds == NULL) ||
		     (strcmp(pds, "sng") == 0) ||
		     ctx->cfg.uud) &&
		    (use_iov == 1)) {
			continue;
		}

		ctx->cfg.iov_test = (use_iov == 1);
		printf("Starting in %s\n",
				(use_iov == 1) ? "iov_mode" : "buf_mode");
		first = true;

		/* perform control message exchange */
		if (ctx->cfg.rma || ctx->cfg.sync_rma ||
		    ctx->cfg.atomic || ctx->cfg.sync_atomic) {
			if (ctx->cfg.client) {
				rc = uet_rma_client_ctrl_exchange(ctx);
				if (rc != UET_SUCCESS_RC)
					goto exit;
			} else {
				rc = uet_rma_server_ctrl_exchange(ctx);
				if (rc != UET_SUCCESS_RC)
					goto exit;
			}
		}

		/* perform data transfer */
		for (iteration = 0; iteration < ctx->cfg.num_iterations;
		     iteration++) {
			if (ctx->cfg.client) {
				if (ctx->cfg.uud) {
					rc = uet_uud_client(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.rma) {
					rc = uet_rma_client(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.sync_rma) {
					rc = uet_sync_rma_client(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.atomic) {
					rc = uet_atomic_client(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.sync_atomic) {
					rc = uet_sync_atomic_client(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else {
					if (ctx->cfg.unexpected_msg_test)
						rc = uet_msg_client_unexpected(
									   ctx);
					else
						rc = uet_msg_client(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				}
			} else { /* server */
				if (ctx->cfg.uud) {
					rc = uet_uud_server(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.rma) {
					rc = uet_rma_server(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.sync_rma) {
					rc = uet_sync_rma_server(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.atomic) {
					rc = uet_atomic_server(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else if (ctx->cfg.sync_atomic) {
					rc = uet_sync_atomic_server(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				} else {
					rc = uet_msg_server(ctx);
					if (rc != UET_SUCCESS_RC)
						goto exit;
				}
			}
		}
	}

exit:
	printf("Completed %d iterations\n", iteration);
	uet_free_res(ctx); /* free resources */
	return rc;
}

int main(int argc, char *argv[])
{
	int rc, test_argc;
	char *test_argv[UET_MAX_ARGS+1];
	struct uet_context *ctx = &uet_ctx;
	bool do_all = false;
	char *cmd, *c_s, *test, *ip;

	if (argc != 4)  {
		UET_ERR("Invalid usage: wrong number of args");
		UET_USAGE(argv[0]);
		return UET_ERR_RC;
	}

	if (strcmp(argv[1], "client") == 0)
		ctx->cfg.client = true;

	if (strcmp(argv[2], "all") == 0)
		do_all = true;

	cmd  = argv[0];
	c_s  = argv[1];
	test = argv[2];
	ip   = argv[3];

	if (do_all || (strcmp(test, "rma") == 0)) {
		printf("\nRMA Test\n");
		printf(  "========\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "rma";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "sync_rma") == 0)) {
		printf("\nSync Group RMA Test\n");
		printf(  "===================\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "sync_rma";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "untag") == 0)) {
		printf("\nUntagged Message Test\n");
		printf(  "=====================\n");
		test_argc = 3;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = ip;
		test_argv[3] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "tag") == 0)) {
		printf("\nTagged Message Test\n");
		printf(  "===================\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "tag";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "tag_any_src") == 0)) {
		printf("\nTagged Message Test (Any Source)\n");
		printf(  "================================\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "tag";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));
		ctx->cfg.tag_any_src = true;

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "unexp_untag") == 0)) {
		printf("\nUnexpected Untagged Message Test\n");
		printf(  "================================\n");
		test_argc = 3;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = ip;
		test_argv[3] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));
		ctx->cfg.unexpected_msg_test = true;

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "unexp_tag") == 0)) {
		printf("\nUnexpected Tagged Message Test\n");
		printf(  "==============================\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "tag";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));
		ctx->cfg.unexpected_msg_test = true;

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "defer_send") == 0)) {
		printf("\nDeferred Send Message Test\n");
		printf(  "==========================\n");
		test_argc = 3;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = ip;
		test_argv[3] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));
		ctx->cfg.unexpected_msg_test = true;
		ctx->cfg.dsend_test = true;

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "defer_tag") == 0)) {
		printf("\nDeferred Tagged Send Message Test\n");
		printf(  "=================================\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "tag";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));
		ctx->cfg.unexpected_msg_test = true;
		ctx->cfg.dsend_test = true;

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "defer_tag_any_src") == 0)) {
		printf("\nDeferred Tagged Send Message Test (Any Source)\n");
		printf(  "==============================================\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "tag";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));
		ctx->cfg.unexpected_msg_test = true;
		ctx->cfg.dsend_test = true;
		ctx->cfg.tag_any_src = true;

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "atomic") == 0)) {
		printf("\nAtomic Test\n");
		printf(  "===========\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "atomic";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	if (do_all || (strcmp(test, "sync_atomic") == 0)) {
		printf("\nSync Group Atomic Test\n");
		printf(  "======================\n");
		test_argc = 4;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = "sync_atomic";
		test_argv[3] = ip;
		test_argv[4] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	/* UUD is standalone (not in 'all'): it needs UET_FORCE_UUD + the pds
	 * backend and is a best-effort single-packet datagram send.
	 */
	if (strcmp(test, "uud") == 0) {
		printf("\nUUD Datagram Test\n");
		printf(  "=================\n");
		test_argc = 3;
		test_argv[0] = cmd;
		test_argv[1] = c_s;
		test_argv[2] = ip;
		test_argv[3] = NULL;
		memset(ctx, 0, sizeof(struct uet_context));

		rc = uet_run(test_argc, test_argv, ctx);
		if (rc != UET_SUCCESS_RC)
			exit(rc);
	}

	exit(0);
}
