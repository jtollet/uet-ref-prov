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

static int check(int rc, const char *operation)
{
	if (!rc)
		return 0;
	fprintf(stderr, "%s: %s (%d)\n", operation, fi_strerror(-rc), rc);
	return rc;
}

int main(int argc, char **argv)
{
	struct fi_info *hints = NULL, *info = NULL;
	struct fid_fabric *fabric = NULL;
	struct fid_eq *eq = NULL;
	struct fid_domain *domain = NULL;
	struct fid_av *av = NULL;
	struct fid_cq *cq = NULL;
	struct fid_ep *ep = NULL;
	struct fid_mr *mr = NULL;
	struct fi_av_attr av_attr = { .type = FI_AV_TABLE, .count = 16 };
	struct fi_eq_attr eq_attr = { .wait_obj = FI_WAIT_NONE };
	struct fi_cq_attr cq_attr = {
		.size = 32,
		.format = FI_CQ_FORMAT_TAGGED,
		.wait_obj = FI_WAIT_NONE,
	};
	struct uet_addr local_addr;
	size_t addrlen = sizeof(local_addr);
	char buffer[4096];
	int close_rc;
	int rc = 1;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <local-ip>\n", argv[0]);
		return 2;
	}

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

	/* Exercise normal FI_MR_ENDPOINT cleanup before endpoint close. */
	fprintf(stderr, "smoke: fi_close(MR)\n");
	rc = fi_close(&mr->fid);
	if (check(rc, "fi_close(MR)"))
		goto out;
	mr = NULL;

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
