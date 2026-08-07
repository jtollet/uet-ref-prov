/*
 * Copyright (c) 2026 Cisco Systems, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>

#include "uet_addr.h"

#define UET_PROVIDER_SMOKE_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static int check(int rc, const char *operation)
{
	if (!rc)
		return 0;
	fprintf(stderr, "%s: %s (%d)\n", operation, fi_strerror(-rc), rc);
	return rc;
}

static int check_threading_hints(const char *node)
{
	static const enum fi_threading models[] = {
		FI_THREAD_SAFE,
		FI_THREAD_FID,
		FI_THREAD_DOMAIN,
		FI_THREAD_COMPLETION,
		FI_THREAD_ENDPOINT,
	};
	struct fi_info *hints;
	struct fi_info *info;
	size_t i;
	int rc;

	for (i = 0; i < UET_PROVIDER_SMOKE_ARRAY_SIZE(models); i++) {
		hints = fi_allocinfo();
		if (!hints)
			return -FI_ENOMEM;
		hints->caps = FI_MSG | FI_TAGGED;
		hints->ep_attr->type = FI_EP_RDM;
		hints->domain_attr->threading = models[i];
		hints->fabric_attr->prov_name = strdup("uet");
		if (!hints->fabric_attr->prov_name) {
			fi_freeinfo(hints);
			return -FI_ENOMEM;
		}

		info = NULL;
		rc = fi_getinfo(FI_VERSION(1, 20), node, NULL, FI_SOURCE,
				hints, &info);
		fi_freeinfo(info);
		fi_freeinfo(hints);
		if (rc)
			return rc;
	}

	return FI_SUCCESS;
}

int main(int argc, char **argv)
{
	struct fi_info *hints = NULL, *info = NULL;
	struct fid_fabric *fabric = NULL;
	struct fid_eq *eq = NULL;
	struct fid_domain *domain = NULL;
	struct fid_av *av = NULL;
	struct fid_cq *cq = NULL;
	struct fid_ep *ep = NULL, *ep2 = NULL, *ep3 = NULL;
	struct fid_mr *mr = NULL;
	struct fi_av_attr av_attr = { .type = FI_AV_TABLE, .count = 16 };
	struct fi_eq_attr eq_attr = { .wait_obj = FI_WAIT_NONE };
	struct fi_cq_attr cq_attr = {
		.size = 32,
		.format = FI_CQ_FORMAT_TAGGED,
		.wait_obj = FI_WAIT_NONE,
	};
	struct uet_addr local_addr, local_addr2, local_addr3;
	size_t addrlen = sizeof(local_addr);
	char buffer[4096];
	int close_rc;
	int rc = 1;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <local-ip>\n", argv[0]);
		return 2;
	}

	fprintf(stderr, "smoke: threading hints\n");
	rc = check_threading_hints(argv[1]);
	if (check(rc, "fi_getinfo(threading hints)"))
		return 1;

	hints = fi_allocinfo();
	if (!hints)
		return 1;
	hints->caps = FI_MSG | FI_TAGGED;
	hints->ep_attr->type = FI_EP_RDM;
	hints->fabric_attr->prov_name = strdup("uet");
	if (!hints->fabric_attr->prov_name)
		goto out;

	fprintf(stderr, "smoke: fi_getinfo\n");
	rc = fi_getinfo(FI_VERSION(1, 20), argv[1], NULL, FI_SOURCE,
			hints, &info);
	if (check(rc, "fi_getinfo"))
		goto out;
	fprintf(stderr, "smoke: fi_fabric\n");
	rc = fi_fabric(info->fabric_attr, &fabric, NULL);
	if (check(rc, "fi_fabric"))
		goto out;
	fprintf(stderr, "smoke: fi_eq_open\n");
	rc = fi_eq_open(fabric, &eq_attr, &eq, NULL);
	if (check(rc, "fi_eq_open"))
		goto out;
	fprintf(stderr, "smoke: fi_domain\n");
	rc = fi_domain(fabric, info, &domain, NULL);
	if (check(rc, "fi_domain"))
		goto out;
	fprintf(stderr, "smoke: fi_av_open\n");
	rc = fi_av_open(domain, &av_attr, &av, NULL);
	if (check(rc, "fi_av_open"))
		goto out;
	fprintf(stderr, "smoke: fi_cq_open\n");
	rc = fi_cq_open(domain, &cq_attr, &cq, NULL);
	if (check(rc, "fi_cq_open"))
		goto out;
	fprintf(stderr, "smoke: fi_endpoint\n");
	rc = fi_endpoint(domain, info, &ep, NULL);
	if (check(rc, "fi_endpoint"))
		goto out;
	{
		size_t inject_size = 0;
		size_t optlen = sizeof(inject_size);

		fprintf(stderr, "smoke: unsupported endpoint options\n");
		rc = fi_getopt(&ep->fid, FI_OPT_ENDPOINT,
			       FI_OPT_INJECT_MSG_SIZE, &inject_size, &optlen);
		if (rc != -FI_ENOPROTOOPT) {
			fprintf(stderr,
				"fi_getopt(inject size): expected %d, got %d\n",
				-FI_ENOPROTOOPT, rc);
			goto out;
		}
		rc = fi_setopt(&ep->fid, FI_OPT_ENDPOINT,
			       FI_OPT_INJECT_MSG_SIZE, &inject_size,
			       sizeof(inject_size));
		if (rc != -FI_ENOPROTOOPT) {
			fprintf(stderr,
				"fi_setopt(inject size): expected %d, got %d\n",
				-FI_ENOPROTOOPT, rc);
			goto out;
		}
		rc = FI_SUCCESS;
	}
	fprintf(stderr, "smoke: fi_ep_bind(AV)\n");
	rc = fi_ep_bind(ep, &av->fid, 0);
	if (check(rc, "fi_ep_bind(AV)"))
		goto out;
	fprintf(stderr, "smoke: fi_ep_bind(CQ)\n");
	rc = fi_ep_bind(ep, &cq->fid, FI_SEND | FI_RECV);
	if (check(rc, "fi_ep_bind(CQ)"))
		goto out;
	fprintf(stderr, "smoke: fi_mr_reg\n");
	rc = fi_mr_reg(domain, buffer, sizeof(buffer), FI_SEND | FI_RECV |
		       FI_REMOTE_READ | FI_REMOTE_WRITE, 0, 0, 0, &mr, NULL);
	if (check(rc, "fi_mr_reg"))
		goto out;
	fprintf(stderr, "smoke: fi_mr_bind\n");
	rc = fi_mr_bind(mr, &ep->fid, FI_REMOTE_READ | FI_REMOTE_WRITE);
	if (check(rc, "fi_mr_bind"))
		goto out;
	fprintf(stderr, "smoke: fi_mr_enable\n");
	rc = fi_mr_enable(mr);
	if (check(rc, "fi_mr_enable"))
		goto out;
	fprintf(stderr, "smoke: fi_enable\n");
	rc = fi_enable(ep);
	if (check(rc, "fi_enable"))
		goto out;
	fprintf(stderr, "smoke: fi_getname\n");
	rc = fi_getname(&ep->fid, &local_addr, &addrlen);
	if (check(rc, "fi_getname"))
		goto out;

	/* A single process may create several endpoints on the same fabric
	 * address and PIDonFEP.  They must receive distinct Resource Indices.
	 */
	fprintf(stderr, "smoke: fi_endpoint(second)\n");
	rc = fi_endpoint(domain, info, &ep2, NULL);
	if (check(rc, "fi_endpoint(second)"))
		goto out;
	addrlen = sizeof(local_addr2);
	rc = fi_getname(&ep2->fid, &local_addr2, &addrlen);
	if (check(rc, "fi_getname(second)"))
		goto out;
	if (local_addr.pid_on_fep != local_addr2.pid_on_fep ||
	    local_addr.start_index == local_addr2.start_index) {
		fprintf(stderr,
			"multi-endpoint address allocation failed: PID %u, RI %u/%u\n",
			local_addr.pid_on_fep, local_addr.start_index,
			local_addr2.start_index);
		rc = -FI_EADDRINUSE;
		goto out;
	}

	/* Exercise normal FI_MR_ENDPOINT cleanup before endpoint close. */
	fprintf(stderr, "smoke: fi_close(MR)\n");
	rc = fi_close(&mr->fid);
	if (check(rc, "fi_close(MR)"))
		goto out;
	mr = NULL;

	/* The engine waits for its close lifetime before removing the address.
	 * Once close returns, the index may be allocated again without colliding
	 * with the still-live second endpoint.
	 */
	fprintf(stderr, "smoke: fi_close(first EP)\n");
	rc = fi_close(&ep->fid);
	if (check(rc, "fi_close(first EP)"))
		goto out;
	ep = NULL;
	fprintf(stderr, "smoke: fi_endpoint(reallocated)\n");
	rc = fi_endpoint(domain, info, &ep3, NULL);
	if (check(rc, "fi_endpoint(reallocated)"))
		goto out;
	addrlen = sizeof(local_addr3);
	rc = fi_getname(&ep3->fid, &local_addr3, &addrlen);
	if (check(rc, "fi_getname(reallocated)"))
		goto out;
	if (local_addr3.start_index != local_addr.start_index) {
		fprintf(stderr, "closed Resource Index was not reused: %u != %u\n",
			local_addr3.start_index, local_addr.start_index);
		rc = -FI_EINVAL;
		goto out;
	}

	rc = 0;

out:
	fprintf(stderr, "smoke: cleanup\n");
	if (mr) {
		close_rc = fi_close(&mr->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(MR)");
	}
	if (ep) {
		close_rc = fi_close(&ep->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(EP)");
	}
	if (ep3) {
		close_rc = fi_close(&ep3->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(reallocated EP)");
	}
	if (ep2) {
		close_rc = fi_close(&ep2->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(second EP)");
	}
	if (cq) {
		close_rc = fi_close(&cq->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(CQ)");
	}
	if (av) {
		close_rc = fi_close(&av->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(AV)");
	}
	if (eq) {
		close_rc = fi_close(&eq->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(EQ)");
	}
	if (domain) {
		close_rc = fi_close(&domain->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(domain)");
	}
	if (fabric) {
		close_rc = fi_close(&fabric->fid);
		if (close_rc && !rc)
			rc = check(close_rc, "fi_close(fabric)");
	}
	fi_freeinfo(info);
	fi_freeinfo(hints);
	if (!rc)
		printf("uet provider smoke test passed (%s backend)\n",
		       getenv("UET_NIC_SHIM") ? getenv("UET_NIC_SHIM") :
						 "rawsock");
	return rc ? 1 : 0;
}
