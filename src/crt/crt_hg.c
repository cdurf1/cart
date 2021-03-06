/* Copyright (C) 2016 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * This file is part of CaRT. It implements the main interfaces to mercury.
 */

#include <crt_internal.h>
#include <abt.h>

static hg_return_t
hg_addr_lookup_cb(const struct hg_cb_info *callback_info)
{
	hg_addr_t	*addr_ptr = (hg_addr_t *) callback_info->arg;
	hg_return_t	ret = HG_SUCCESS;

	if (callback_info->ret != HG_SUCCESS) {
		C_ERROR("Return from callback with %s error code",
			HG_Error_to_string(callback_info->ret));
		return ret;
	}

	*addr_ptr = callback_info->info.lookup.addr;

	return ret;
}

/* connection timeout 10 second */
#define CRT_CONNECT_TIMEOUT_SEC		(10)

int
crt_hg_addr_lookup_wait(hg_class_t *hg_class, hg_context_t *hg_context,
			const char *name, hg_addr_t *addr)
{
	hg_addr_t		new_addr = NULL;
	uint64_t		now;
	uint64_t		end;
	unsigned int		prog_msec;
	hg_return_t		ret = HG_SUCCESS;
	int			rc = 0;

	C_ASSERT(hg_context != NULL);
	C_ASSERT(hg_class != NULL);
	C_ASSERT(name != NULL);
	C_ASSERT(addr != NULL);

	ret = HG_Addr_lookup(hg_context, &hg_addr_lookup_cb, &new_addr, name,
			HG_OP_ID_IGNORE);
	if (ret != HG_SUCCESS) {
		C_ERROR("Could not start HG_Addr_lookup");
		C_GOTO(done, rc = -CER_HG);
	}

	end = crt_timeus_secdiff(CRT_CONNECT_TIMEOUT_SEC);
	prog_msec = 1;

	while (1) {
		hg_return_t	trigger_ret;
		unsigned int	actual_count = 0;

		do {
			trigger_ret = HG_Trigger(hg_context, 0, 1,
						 &actual_count);
		} while ((trigger_ret == HG_SUCCESS) && actual_count);

		if (new_addr != NULL) {
			*addr = new_addr;
			break;
		}

		ret = HG_Progress(hg_context, prog_msec);
		if (ret != HG_SUCCESS && ret != HG_TIMEOUT) {
			C_ERROR("Could not make progress");
			rc = -CER_HG;
			break;
		}

		now = crt_timeus_secdiff(0);
		if (now >= end) {
			char		my_host[CRT_ADDR_STR_MAX_LEN] = {'\0'};
			crt_rank_t	my_rank;

			crt_group_rank(NULL, &my_rank);
			gethostname(my_host, CRT_ADDR_STR_MAX_LEN);

			C_ERROR("Could not connect to %s within %d second "
				"(rank %d, host %s).\n", name,
				CRT_CONNECT_TIMEOUT_SEC, my_rank, my_host);
			rc = -CER_TIMEDOUT;
			break;
		}

		if (prog_msec <= 512)
			prog_msec = prog_msec << 1;
	}

done:
	C_ASSERT(new_addr != NULL || rc != 0);
	return rc;
}

static int
na_class_get_addr(na_class_t *na_class, char *addr_str, crt_size_t *str_size)
{
	na_addr_t	self_addr;
	na_return_t	na_ret;
	int		rc = 0;

	C_ASSERT(na_class != NULL);
	C_ASSERT(addr_str != NULL && str_size != NULL);

	na_ret = NA_Addr_self(na_class, &self_addr);
	if (na_ret != NA_SUCCESS) {
		C_ERROR("NA_Addr_self failed, na_ret: %d.\n", na_ret);
		C_GOTO(out, rc = -CER_HG);
	}

	na_ret = NA_Addr_to_string(na_class, addr_str, str_size, self_addr);
	if (na_ret != NA_SUCCESS) {
		C_ERROR("NA_Addr_to_string failed, na_ret: %d.\n",
			na_ret);
		NA_Addr_free(na_class, self_addr);
		C_GOTO(out, rc = -CER_HG);
	}
	NA_Addr_free(na_class, self_addr);

out:
	return rc;
}

/* be called only in crt_init */
int
crt_hg_init(crt_phy_addr_t *addr, bool server)
{
	const char		*info_string;
	struct crt_hg_gdata	*hg_gdata;
	na_class_t		*na_class = NULL;
	hg_class_t		*hg_class = NULL;
	int			rc = 0;

	if (crt_initialized()) {
		C_ERROR("CaRT already initialized.\n");
		C_GOTO(out, rc = -CER_ALREADY);
	}

	if (*addr != NULL) {
		info_string = *addr;
		C_ASSERT(strncmp(info_string, "bmi+tcp", 7) == 0);
	} else {
		if (crt_gdata.cg_verbs == true)
			info_string = "cci+verbs://";
		else
			info_string = "cci+tcp://";
	}

	na_class = NA_Initialize(info_string, server);
	if (na_class == NULL) {
		C_ERROR("Could not initialize NA class.\n");
		C_GOTO(out, rc = -CER_HG);
	}

	hg_class = HG_Init_na(na_class);
	if (hg_class == NULL) {
		C_ERROR("Could not initialize HG class.\n");
		NA_Finalize(na_class);
		C_GOTO(out, rc = -CER_HG);
	}

	C_ALLOC_PTR(hg_gdata);
	if (hg_gdata == NULL) {
		HG_Finalize(hg_class);
		NA_Finalize(na_class);
		C_GOTO(out, rc = -CER_NOMEM);
	}

	hg_gdata->chg_nacla = na_class;
	hg_gdata->chg_hgcla = hg_class;

	crt_gdata.cg_hg = hg_gdata;

	/* register the CRT_HG_RPCID */
	rc = crt_hg_reg(crt_gdata.cg_hg->chg_hgcla, CRT_HG_RPCID,
			(crt_proc_cb_t)crt_proc_in_common,
			(crt_proc_cb_t)crt_proc_out_common,
			(crt_hg_rpc_cb_t)crt_rpc_handler_common);
	if (rc != 0) {
		C_ERROR("crt_hg_reg(rpcid: 0x%x), failed rc: %d.\n",
			CRT_HG_RPCID, rc);
		HG_Finalize(hg_class);
		NA_Finalize(na_class);
		C_GOTO(out, rc = -CER_HG);
	}

	if (*addr == NULL) {
		char		addr_str[CRT_ADDR_STR_MAX_LEN] = {'\0'};
		crt_size_t	str_size = CRT_ADDR_STR_MAX_LEN;

		rc = na_class_get_addr(na_class, addr_str, &str_size);
		if (rc != 0) {
			C_ERROR("na_class_get_addr failed, rc: %d.\n", rc);
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			C_GOTO(out, rc = -CER_HG);
		}

		*addr = strdup(addr_str);
		if (*addr == NULL) {
			C_ERROR("strdup failed, rc: %d.\n", rc);
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			C_GOTO(out, rc = -CER_HG);
		}
	}

	C_DEBUG("in crt_hg_init, listen address: %s.\n", *addr);

out:
	return rc;
}

/* be called only in crt_finalize */
int
crt_hg_fini()
{
	na_class_t	*na_class;
	hg_class_t	*hg_class;
	hg_return_t	hg_ret = HG_SUCCESS;
	na_return_t	na_ret = NA_SUCCESS;
	int		rc = 0;

	if (!crt_initialized()) {
		C_ERROR("CaRT not initialized.\n");
		C_GOTO(out, rc = -CER_NO_PERM);
	}

	na_class = crt_gdata.cg_hg->chg_nacla;
	hg_class = crt_gdata.cg_hg->chg_hgcla;
	C_ASSERT(na_class != NULL);
	C_ASSERT(hg_class != NULL);

	hg_ret = HG_Finalize(hg_class);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("Could not finalize HG class, hg_ret: %d.\n", hg_ret);
		C_GOTO(out, rc = -CER_HG);
	}

	na_ret = NA_Finalize(na_class);
	if (na_ret != NA_SUCCESS) {
		C_ERROR("Could not finalize NA class, na_ret: %d.\n", na_ret);
		C_GOTO(out, rc = -CER_HG);
	}

	C_FREE_PTR(crt_gdata.cg_hg);

out:
	return rc;
}

int
crt_hg_ctx_init(struct crt_hg_context *hg_ctx, int idx)
{
	struct crt_context	*crt_ctx;
	na_class_t		*na_class = NULL;
	hg_class_t		*hg_class = NULL;
	hg_context_t		*hg_context = NULL;
	const char		*info_string;
	hg_return_t		 hg_ret;
	int			 rc = 0;

	C_ASSERT(hg_ctx != NULL);
	crt_ctx = container_of(hg_ctx, struct crt_context, cc_hg_ctx);

	if (idx == 0 || crt_gdata.cg_multi_na == false) {
		/* register crt_ctx to get it in crt_rpc_handler_common */
		hg_ret = HG_Register_data(crt_gdata.cg_hg->chg_hgcla,
					  CRT_HG_RPCID, crt_ctx, NULL);
		if (hg_ret != HG_SUCCESS) {
			C_ERROR("HG_Register_data failed, hg_ret: %d.\n",
				hg_ret);
			C_GOTO(out, rc = -CER_HG);
		}

		hg_context = HG_Context_create(crt_gdata.cg_hg->chg_hgcla);
		if (hg_context == NULL) {
			C_ERROR("Could not create HG context.\n");
			C_GOTO(out, rc = -CER_HG);
		}

		hg_ctx->chc_nacla = crt_gdata.cg_hg->chg_nacla;
		hg_ctx->chc_hgcla = crt_gdata.cg_hg->chg_hgcla;
		hg_ctx->chc_shared_na = true;
	} else {
		char		addr_str[CRT_ADDR_STR_MAX_LEN] = {'\0'};
		crt_size_t	str_size = CRT_ADDR_STR_MAX_LEN;

		if (crt_gdata.cg_verbs == true)
			info_string = "cci+verbs://";
		else
			info_string = "cci+tcp://";

		na_class = NA_Initialize(info_string, crt_is_service());
		if (na_class == NULL) {
			C_ERROR("Could not initialize NA class.\n");
			C_GOTO(out, rc = -CER_HG);
		}

		rc = na_class_get_addr(na_class, addr_str, &str_size);
		if (rc != 0) {
			C_ERROR("na_class_get_addr failed, rc: %d.\n", rc);
			NA_Finalize(na_class);
			C_GOTO(out, rc = -CER_HG);
		}
		C_DEBUG("New context(idx:%d), listen address: cci+%s.\n",
			idx, addr_str);

		hg_class = HG_Init_na(na_class);
		if (hg_class == NULL) {
			C_ERROR("Could not initialize HG class.\n");
			NA_Finalize(na_class);
			C_GOTO(out, rc = -CER_HG);
		}

		hg_context = HG_Context_create(hg_class);
		if (hg_context == NULL) {
			C_ERROR("Could not create HG context.\n");
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			C_GOTO(out, rc = -CER_HG);
		}

		/* register the shared RPCID to every hg_class */
		rc = crt_hg_reg(hg_class, CRT_HG_RPCID,
				(crt_proc_cb_t)crt_proc_in_common,
				(crt_proc_cb_t)crt_proc_out_common,
				(crt_hg_rpc_cb_t)crt_rpc_handler_common);
		if (rc != 0) {
			C_ERROR("crt_hg_reg(rpcid: 0x%x), failed rc: %d.\n",
				CRT_HG_RPCID, rc);
			HG_Context_destroy(hg_context);
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			C_GOTO(out, rc = -CER_HG);
		}

		/* register crt_ctx to get it in crt_rpc_handler_common */
		hg_ret = HG_Register_data(hg_class, CRT_HG_RPCID, crt_ctx,
					  NULL);
		if (hg_ret != HG_SUCCESS) {
			C_ERROR("HG_Register_data failed, hg_ret: %d.\n",
				hg_ret);
			HG_Context_destroy(hg_context);
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			C_GOTO(out, rc = -CER_HG);
		}

		hg_ctx->chc_nacla = na_class;
		hg_ctx->chc_hgcla = hg_class;
		hg_ctx->chc_shared_na = false;
	}

	hg_ctx->chc_hgctx = hg_context;
	/* TODO: need to create separate bulk class and bulk context? */
	hg_ctx->chc_bulkcla = hg_ctx->chc_hgcla;
	hg_ctx->chc_bulkctx = hg_ctx->chc_hgctx;
	C_ASSERT(hg_ctx->chc_bulkcla != NULL);
	C_ASSERT(hg_ctx->chc_bulkctx != NULL);

out:
	return rc;
}

int
crt_hg_ctx_fini(struct crt_hg_context *hg_ctx)
{
	hg_context_t	*hg_context;
	hg_return_t	hg_ret = HG_SUCCESS;
	na_return_t	na_ret;
	int		rc = 0;

	C_ASSERT(hg_ctx != NULL);
	hg_context = hg_ctx->chc_hgctx;
	C_ASSERT(hg_context != NULL);

	hg_ret = HG_Context_destroy(hg_context);
	if (hg_ret == HG_SUCCESS) {
		hg_ctx->chc_hgctx = NULL;
	} else {
		C_ERROR("Could not destroy HG context, hg_ret: %d.\n", hg_ret);
		C_GOTO(out, rc = -CER_HG);
	}

	if (hg_ctx->chc_shared_na == true)
		goto out;

	/* the hg_context destroyed, ignore below errors with error logging */
	hg_ret = HG_Finalize(hg_ctx->chc_hgcla);
	if (hg_ret != HG_SUCCESS)
		C_ERROR("Could not finalize HG class, hg_ret: %d.\n", hg_ret);

	na_ret = NA_Finalize(hg_ctx->chc_nacla);
	if (na_ret != NA_SUCCESS)
		C_ERROR("Could not finalize NA class, na_ret: %d.\n", na_ret);

out:
	return rc;
}

struct crt_context *
crt_hg_context_lookup(hg_context_t *hg_ctx)
{
	struct crt_context	*crt_ctx;
	int			found = 0;

	pthread_rwlock_rdlock(&crt_gdata.cg_rwlock);

	crt_list_for_each_entry(crt_ctx, &crt_gdata.cg_ctx_list, cc_link) {
		if (crt_ctx->cc_hg_ctx.chc_hgctx == hg_ctx) {
			found = 1;
			break;
		}
	}

	pthread_rwlock_unlock(&crt_gdata.cg_rwlock);

	return (found == 1) ? crt_ctx : NULL;
}

int
crt_rpc_handler_common(hg_handle_t hg_hdl)
{
	struct crt_context	*crt_ctx;
	struct crt_hg_context	*hg_ctx;
	struct hg_info		*hg_info;
	struct crt_rpc_priv	*rpc_priv;
	crt_rpc_t		*rpc_pub;
	crt_opcode_t		 opc;
	crt_proc_t		 proc = NULL;
	struct crt_opc_info	*opc_info = NULL;
	hg_return_t		 hg_ret = HG_SUCCESS;
	bool			 is_coll_req = false;
	int			 rc = 0;

	hg_info = HG_Get_info(hg_hdl);
	if (hg_info == NULL) {
		C_ERROR("HG_Get_info failed.\n");
		C_GOTO(out, hg_ret = HG_PROTOCOL_ERROR);
	}

	crt_ctx = (struct crt_context *)HG_Registered_data(
			hg_info->hg_class, CRT_HG_RPCID);
	if (crt_ctx == NULL) {
		C_ERROR("HG_Registered_data failed.\n");
		C_GOTO(out, hg_ret = HG_PROTOCOL_ERROR);
	}
	hg_ctx = &crt_ctx->cc_hg_ctx;
	C_ASSERT(hg_ctx->chc_hgcla == hg_info->hg_class);
	C_ASSERT(hg_ctx->chc_hgctx == hg_info->context);

	C_ALLOC_PTR(rpc_priv);
	if (rpc_priv == NULL)
		C_GOTO(out, hg_ret = HG_NOMEM_ERROR);

	rpc_priv->crp_na_addr = hg_info->addr;
	rpc_priv->crp_hg_hdl = hg_hdl;
	rpc_pub = &rpc_priv->crp_pub;
	rpc_pub->cr_ctx = crt_ctx;
	C_ASSERT(rpc_pub->cr_input == NULL);

	rc = crt_hg_unpack_header(rpc_priv, &proc);
	if (rc != 0) {
		C_ERROR("crt_hg_unpack_header failed, rc: %d.\n", rc);
		C_FREE_PTR(rpc_priv);
		C_GOTO(out, hg_ret = HG_OTHER_ERROR);
	}
	if (rpc_priv->crp_flags & CRT_RPC_FLAG_COLL) {
		is_coll_req = true;
		rpc_priv->crp_input_got = 1;
	}
	C_ASSERT(proc != NULL);
	opc = rpc_priv->crp_req_hdr.cch_opc;

	opc_info = crt_opc_lookup(crt_gdata.cg_opc_map, opc, CRT_UNLOCK);
	if (opc_info == NULL) {
		C_ERROR("opc: 0x%x, lookup failed.\n", opc);
		C_FREE_PTR(rpc_priv);
		crt_hg_unpack_cleanup(proc);
		C_GOTO(out, hg_ret = HG_NO_MATCH);
	}
	C_ASSERT(opc_info->coi_opc == opc);
	rpc_priv->crp_opc_info = opc_info;

	rc = crt_rpc_priv_init(rpc_priv, crt_ctx, opc, true /* srv_flag */,
			       false /* forward */);
	if (rc != 0) {
		C_ERROR("crt_rpc_priv_init faied, opc: 0x%x, rc: %d.\n",
			opc, rc);
		crt_hg_unpack_cleanup(proc);
		C_GOTO(decref, hg_ret = HG_NOMEM_ERROR);
	}

	C_ASSERT(rpc_priv->crp_srv != 0);
	C_ASSERT(opc_info->coi_input_size == rpc_pub->cr_input_size);
	if (rpc_pub->cr_input_size > 0) {
		C_ASSERT(rpc_pub->cr_input != NULL);
		C_ASSERT(opc_info->coi_crf != NULL);
		/* corresponding to HG_Free_input in crt_hg_req_destroy */
		rc = crt_hg_unpack_body(rpc_priv, proc);
		if (rc == 0) {
			rpc_priv->crp_input_got = 1;
			rpc_pub->cr_ep.ep_rank =
					rpc_priv->crp_req_hdr.cch_rank;
			rpc_pub->cr_ep.ep_grp = NULL;
			/* TODO lookup by rpc_priv->crp_req_hdr.cch_grp_id */
		} else {
			C_ERROR("_unpack_body failed, rc: %d, opc: 0x%x.\n",
				hg_ret, rpc_pub->cr_opc);
			C_GOTO(decref, hg_ret = HG_OTHER_ERROR);
		}
	} else {
		crt_hg_unpack_cleanup(proc);
	}

	if (opc_info->coi_rpc_cb == NULL) {
		C_ERROR("NULL crp_hg_hdl, opc: 0x%x.\n", opc);
		hg_ret = HG_NO_MATCH;
		rc = -CER_UNREG;
		C_GOTO(decref, hg_ret = HG_NO_MATCH);
	}

	if (!is_coll_req)
		rc = crt_rpc_common_hdlr(rpc_priv);
	else
		rc = crt_corpc_common_hdlr(rpc_priv);

decref:
	/* if ABT enabled and the ULT created successfully, the crt_handle_rpc
	 * will decref it. */
	if (rc != 0 || crt_ctx->cc_pool == NULL) {
		rc = crt_req_decref(rpc_pub);
		if (rc != 0)
			C_ERROR("crt_req_decref failed, rc: %d.\n", rc);
	}
out:
	return hg_ret;
}

int
crt_hg_req_create(struct crt_hg_context *hg_ctx, int ctx_idx,
		  crt_endpoint_t tgt_ep, struct crt_rpc_priv *rpc_priv)
{
	struct crt_grp_priv	*grp_priv;
	hg_return_t		hg_ret = HG_SUCCESS;
	int			rc = 0;

	C_ASSERT(hg_ctx != NULL && hg_ctx->chc_hgcla != NULL &&
		 hg_ctx->chc_hgctx != NULL);
	C_ASSERT(rpc_priv != NULL);

	if (tgt_ep.ep_grp == NULL)
		grp_priv = crt_gdata.cg_grp->gg_srv_pri_grp;
	else
		grp_priv = container_of(tgt_ep.ep_grp, struct crt_grp_priv,
					gp_pub);
	rc = crt_grp_lc_lookup(grp_priv, ctx_idx, hg_ctx, tgt_ep.ep_rank,
			       tgt_ep.ep_tag, NULL /* base_addr */,
			       &rpc_priv->crp_na_addr);
	if (rc != 0) {
		C_ERROR("crt_grp_lc_lookup failed, rc: %d, opc: 0x%x.\n",
			rc, rpc_priv->crp_pub.cr_opc);
		C_GOTO(out, rc);
	}

	hg_ret = HG_Create(hg_ctx->chc_hgctx, rpc_priv->crp_na_addr,
			   CRT_HG_RPCID, &rpc_priv->crp_hg_hdl);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("HG_Create failed, hg_ret: %d, opc: 0x%x.\n",
			hg_ret, rpc_priv->crp_pub.cr_opc);
		rc = -CER_HG;
	}

out:
	return rc;
}

int
crt_hg_req_destroy(struct crt_rpc_priv *rpc_priv)
{
	hg_return_t	hg_ret = HG_SUCCESS;
	int		rc = 0;

	C_ASSERT(rpc_priv != NULL);
	if (rpc_priv->crp_output_got != 0) {
		hg_ret = HG_Free_output(rpc_priv->crp_hg_hdl,
					&rpc_priv->crp_pub.cr_output);
		if (hg_ret != HG_SUCCESS)
			C_ERROR("HG_Free_output failed, hg_ret: %d, "
				"opc: 0x%x.\n", hg_ret,
				rpc_priv->crp_pub.cr_opc);
	}
	if (rpc_priv->crp_input_got != 0) {
		hg_ret = HG_Free_input(rpc_priv->crp_hg_hdl,
				       &rpc_priv->crp_pub.cr_input);
		if (hg_ret != HG_SUCCESS)
			C_ERROR("HG_Free_input failed, hg_ret: %d, "
				"opc: 0x%x.\n", hg_ret,
				rpc_priv->crp_pub.cr_opc);
	}

	crt_rpc_priv_fini(rpc_priv);

	if (!rpc_priv->crp_coll &&
	    (!CRT_HG_LOWLEVEL_UNPACK || (rpc_priv->crp_input_got == 0))) {
		/* HACK alert:  Do we need to provide a low-level interface
		 * for HG_Free_input since we do low level packing.   Without
		 * calling HG_Get_input, we don't take a reference on the
		 * handle calling destroy here can result in the handle
		 * getting freed before mercury is done with it
		 */
		hg_ret = HG_Destroy(rpc_priv->crp_hg_hdl);
		if (hg_ret != HG_SUCCESS) {
			C_ERROR("HG_Destroy failed, hg_ret: %d, opc: 0x%x.\n",
				hg_ret, rpc_priv->crp_pub.cr_opc);
		}
	}

	crt_rpc_priv_free(rpc_priv);

	return rc;
}

struct crt_hg_send_cbinfo {
	struct crt_rpc_priv	*rsc_rpc_priv;
	crt_cb_t		rsc_cb;
	void			*rsc_arg;
};

/* the common completion callback for sending RPC request */
static hg_return_t
crt_hg_req_send_cb(const struct hg_cb_info *hg_cbinfo)
{
	struct crt_hg_send_cbinfo	*req_cbinfo;
	struct crt_cb_info		crt_cbinfo;
	crt_rpc_t			*rpc_pub;
	struct crt_rpc_priv		*rpc_priv;
	crt_opcode_t			opc;
	hg_return_t			hg_ret = HG_SUCCESS;
	int				rc = 0;

	req_cbinfo = (struct crt_hg_send_cbinfo *)hg_cbinfo->arg;
	C_ASSERT(req_cbinfo != NULL);
	C_ASSERT(hg_cbinfo->type == HG_CB_FORWARD);

	rpc_priv = req_cbinfo->rsc_rpc_priv;
	C_ASSERT(rpc_priv != NULL);
	rpc_pub = &rpc_priv->crp_pub;
	opc = rpc_pub->cr_opc;

	if (hg_cbinfo->ret != HG_SUCCESS) {
		if (hg_cbinfo->ret == HG_CANCELED) {
			if (rpc_priv->crp_state == RPC_TIMEOUT) {
				C_DEBUG("timed out rpc_priv %p being canceled, "
					"opc: 0x%x.\n", rpc_priv, opc);
				C_GOTO(timeout_abort, rc);
			} else {
				C_DEBUG("request being canceled, opc: 0x%x.\n",
					opc);
				rc = -CER_CANCELED;
			}
		} else {
			C_ERROR("hg_cbinfo->ret: %d.\n", hg_cbinfo->ret);
			rc = -CER_HG;
			hg_ret = hg_cbinfo->ret;
		}
	}

	if (req_cbinfo->rsc_cb == NULL) {
		rpc_priv->crp_state = (hg_cbinfo->ret == HG_CANCELED) ?
				      RPC_CANCELED : RPC_COMPLETED;
		C_GOTO(out, hg_ret);
	}

	if (rc == 0) {
		rpc_priv->crp_state = RPC_REPLY_RECVED;
		/* HG_Free_output in crt_hg_req_destroy */
		hg_ret = HG_Get_output(hg_cbinfo->info.forward.handle,
				       &rpc_pub->cr_output);
		if (hg_ret == HG_SUCCESS) {
			rpc_priv->crp_output_got = 1;
		} else {
			C_ERROR("HG_Get_output failed, hg_ret: %d, opc: "
				"0x%x.\n", hg_ret, opc);
			rc = -CER_HG;
		}
	}

	crt_cbinfo.cci_rpc = rpc_pub;
	crt_cbinfo.cci_arg = req_cbinfo->rsc_arg;
	crt_cbinfo.cci_rc = rc;


	C_ASSERT(req_cbinfo->rsc_cb != NULL);
	rc = req_cbinfo->rsc_cb(&crt_cbinfo);
	if (rc != 0)
		C_ERROR("req_cbinfo->rsc_cb returned %d.\n", rc);

	rpc_priv->crp_state = (hg_cbinfo->ret == HG_CANCELED) ?
			      RPC_CANCELED : RPC_COMPLETED;

out:
	crt_context_req_untrack(rpc_pub);

timeout_abort:
	C_FREE_PTR(req_cbinfo);

	/* corresponding to the refcount taken in crt_rpc_priv_init(). */
	rc = crt_req_decref(rpc_pub);
	if (rc != 0)
		C_ERROR("crt_req_decref failed, rc: %d, opc: 0x%x.\n", rc, opc);

	return hg_ret;
}

int
crt_hg_req_send(struct crt_rpc_priv *rpc_priv)
{
	struct crt_hg_send_cbinfo	*cb_info;
	hg_return_t			hg_ret = HG_SUCCESS;
	void				*hg_in_struct;
	int				rc = 0;

	C_ASSERT(rpc_priv != NULL);

	C_ALLOC_PTR(cb_info);
	if (cb_info == NULL)
		C_GOTO(out, rc = -CER_NOMEM);

	hg_in_struct = &rpc_priv->crp_pub.cr_input;

	cb_info->rsc_rpc_priv = rpc_priv;
	cb_info->rsc_cb = rpc_priv->crp_complete_cb;
	cb_info->rsc_arg = rpc_priv->crp_arg;

	hg_ret = HG_Forward(rpc_priv->crp_hg_hdl, crt_hg_req_send_cb, cb_info,
			    hg_in_struct);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("HG_Forward failed, hg_ret: %d, opc: 0x%x.\n",
			hg_ret, rpc_priv->crp_pub.cr_opc);
		C_FREE_PTR(cb_info);
		rc = -CER_HG;
	}


out:
	return rc;
}

int
crt_hg_req_cancel(struct crt_rpc_priv *rpc_priv)
{
	hg_return_t	hg_ret;
	int		rc = 0;

	C_ASSERT(rpc_priv != NULL);
	if (!rpc_priv->crp_hg_hdl)
		C_GOTO(out, rc = -CER_INVAL);

	hg_ret = HG_Cancel(rpc_priv->crp_hg_hdl);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("crt_hg_req_cancel failed, hg_ret: %d, opc: 0x%x.\n",
			hg_ret, rpc_priv->crp_pub.cr_opc);
		rc = -CER_HG;
	}

out:
	return rc;
}

/* just to release the reference taken at crt_hg_reply_send */
static hg_return_t
crt_hg_reply_send_cb(const struct hg_cb_info *hg_cbinfo)
{
	struct crt_hg_send_cbinfo	*req_cbinfo;
	struct crt_rpc_priv		*rpc_priv;
	hg_return_t			hg_ret = HG_SUCCESS;
	crt_opcode_t			opc;
	int				rc = 0;

	req_cbinfo = (struct crt_hg_send_cbinfo *)hg_cbinfo->arg;
	C_ASSERT(req_cbinfo != NULL && req_cbinfo->rsc_rpc_priv != NULL);

	rpc_priv = req_cbinfo->rsc_rpc_priv;
	opc = rpc_priv->crp_pub.cr_opc;
	hg_ret = hg_cbinfo->ret;
	if (hg_ret != HG_SUCCESS)
		C_ERROR("crt_hg_reply_send_cb, hg_cbinfo->ret: %d, "
			"opc: 0x%x.\n", hg_ret, opc);

	/* corresponding to the crt_req_addref in crt_hg_reply_send */
	rc = crt_req_decref(&rpc_priv->crp_pub);
	if (rc != 0)
		C_ERROR("crt_req_decref failed, rc: %d, opc: 0x%x.\n", rc, opc);

	C_FREE_PTR(req_cbinfo);
	return hg_ret;
}

int
crt_hg_reply_send(struct crt_rpc_priv *rpc_priv)
{
	struct crt_hg_send_cbinfo	*cb_info;
	hg_return_t			hg_ret = HG_SUCCESS;
	void				*hg_out_struct;
	int				rc = 0;

	C_ASSERT(rpc_priv != NULL);

	C_ALLOC_PTR(cb_info);
	if (cb_info == NULL)
		C_GOTO(out, rc = -CER_NOMEM);

	hg_out_struct = &rpc_priv->crp_pub.cr_output;

	cb_info->rsc_rpc_priv = rpc_priv;

	rc = crt_req_addref(&rpc_priv->crp_pub);
	if (rc != 0) {
		C_ERROR("crt_req_addref(rpc_priv: %p) failed, rc: %d.\n",
			rpc_priv, rc);
		C_GOTO(out, rc);
	}
	hg_ret = HG_Respond(rpc_priv->crp_hg_hdl, crt_hg_reply_send_cb, cb_info,
			    hg_out_struct);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("HG_Respond failed, hg_ret: %d, opc: 0x%x.\n",
			hg_ret, rpc_priv->crp_pub.cr_opc);
		C_FREE_PTR(cb_info);
		/* should success as addref above */
		rc = crt_req_decref(&rpc_priv->crp_pub);
		C_ASSERT(rc == 0);
		rc = -CER_HG;
	}

out:
	return rc;
}

static int
crt_hg_trigger(struct crt_hg_context *hg_ctx)
{
	struct crt_context	*crt_ctx;
	hg_context_t		*hg_context;
	hg_return_t		hg_ret = HG_SUCCESS;
	unsigned int		count = 0;

	C_ASSERT(hg_ctx != NULL);
	hg_context = hg_ctx->chc_hgctx;
	crt_ctx = container_of(hg_ctx, struct crt_context, cc_hg_ctx);

	do {
		hg_ret = HG_Trigger(hg_context, 0, UINT32_MAX, &count);
	} while (hg_ret == HG_SUCCESS && count > 0);

	if (hg_ret != HG_TIMEOUT) {
		C_ERROR("HG_Trigger failed, hg_ret: %d.\n", hg_ret);
		return -CER_HG;
	}

	/**
	 * XXX Let's yield to other process anyway, but there
	 * maybe better strategy when there are more use cases
	 */
	if (crt_ctx->cc_pool != NULL)
		ABT_thread_yield();

	return 0;
}

int
crt_hg_progress(struct crt_hg_context *hg_ctx, int64_t timeout)
{
	hg_context_t		*hg_context;
	hg_class_t		*hg_class;
	hg_return_t		hg_ret = HG_SUCCESS;
	unsigned int		hg_timeout;
	int			rc;

	C_ASSERT(hg_ctx != NULL);
	hg_context = hg_ctx->chc_hgctx;
	hg_class = hg_ctx->chc_hgcla;
	C_ASSERT(hg_context != NULL && hg_class != NULL);

	/**
	 * Mercury only supports milli-second timeout and uses an unsigned int
	 */
	if (timeout < 0) {
		hg_timeout = UINT32_MAX;
	} else {
		hg_timeout = timeout / 1000;
		if (hg_timeout == 0)
			hg_timeout = 1;

	}

	rc = crt_hg_trigger(hg_ctx);
	if (rc != 0)
		return rc;

	/** progress RPC execution */
	hg_ret = HG_Progress(hg_context, hg_timeout);
	if (hg_ret == HG_TIMEOUT)
		C_GOTO(out, rc = -CER_TIMEDOUT);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("HG_Progress failed, hg_ret: %d.\n", hg_ret);
		C_GOTO(out, rc = -CER_HG);
	}

	/* some RPCs have progressed, call Trigger again */
	rc = crt_hg_trigger(hg_ctx);

out:
	return rc;
}

#define CRT_HG_IOVN_STACK	(8)
int
crt_hg_bulk_create(struct crt_hg_context *hg_ctx, crt_sg_list_t *sgl,
		   crt_bulk_perm_t bulk_perm, crt_bulk_t *bulk_hdl)
{
	void		**buf_ptrs = NULL;
	void		*buf_ptrs_stack[CRT_HG_IOVN_STACK];
	hg_size_t	*buf_sizes = NULL;
	hg_size_t	buf_sizes_stack[CRT_HG_IOVN_STACK];
	hg_uint8_t	flags;
	hg_bulk_t	hg_bulk_hdl;
	hg_return_t	hg_ret = HG_SUCCESS;
	int		rc = 0, i, allocate;

	C_ASSERT(hg_ctx != NULL && hg_ctx->chc_bulkcla != NULL);
	C_ASSERT(sgl != NULL && bulk_hdl != NULL);
	C_ASSERT(bulk_perm == CRT_BULK_RW || bulk_perm == CRT_BULK_RO);

	flags = (bulk_perm == CRT_BULK_RW) ? HG_BULK_READWRITE :
					     HG_BULK_READ_ONLY;

	if (sgl->sg_nr.num <= CRT_HG_IOVN_STACK) {
		allocate = 0;
		buf_sizes = buf_sizes_stack;
	} else {
		allocate = 1;
		C_ALLOC(buf_sizes, sgl->sg_nr.num * sizeof(hg_size_t));
		if (buf_sizes == NULL)
			C_GOTO(out, rc = -CER_NOMEM);
	}
	for (i = 0; i < sgl->sg_nr.num; i++)
		buf_sizes[i] = sgl->sg_iovs[i].iov_buf_len;

	if (sgl->sg_iovs == NULL) {
		buf_ptrs = NULL;
	} else {
		if (allocate == 0) {
			buf_ptrs = buf_ptrs_stack;
		} else {
			C_ALLOC(buf_ptrs, sgl->sg_nr.num * sizeof(void *));
			if (buf_ptrs == NULL)
				C_GOTO(out, rc = -CER_NOMEM);
		}
		for (i = 0; i < sgl->sg_nr.num; i++)
			buf_ptrs[i] = sgl->sg_iovs[i].iov_buf;
	}

	hg_ret = HG_Bulk_create(hg_ctx->chc_bulkcla, sgl->sg_nr.num, buf_ptrs,
				buf_sizes, flags, &hg_bulk_hdl);
	if (hg_ret == HG_SUCCESS) {
		*bulk_hdl = hg_bulk_hdl;
	} else {
		C_ERROR("HG_Bulk_create failed, hg_ret: %d.\n", hg_ret);
		rc = -CER_HG;
	}

out:
	/* HG_Bulk_create copied the parameters, can free here */
	if (allocate == 1 && buf_ptrs != NULL)
		C_FREE(buf_ptrs, sgl->sg_nr.num * sizeof(void *));
	if (allocate == 1 && buf_sizes != NULL)
		C_FREE(buf_sizes, sgl->sg_nr.num * sizeof(hg_size_t));

	return rc;
}

int
crt_hg_bulk_access(crt_bulk_t bulk_hdl, crt_sg_list_t *sgl)
{
	unsigned int	bulk_sgnum;
	unsigned int	actual_sgnum;
	crt_size_t	bulk_len;
	void		**buf_ptrs = NULL;
	void		*buf_ptrs_stack[CRT_HG_IOVN_STACK];
	hg_size_t	*buf_sizes = NULL;
	hg_size_t	buf_sizes_stack[CRT_HG_IOVN_STACK];
	hg_bulk_t	hg_bulk_hdl;
	hg_return_t	hg_ret = HG_SUCCESS;
	int		rc = 0, i, allocate = 0;

	C_ASSERT(bulk_hdl != CRT_BULK_NULL && sgl != NULL);

	rc = crt_bulk_get_sgnum(bulk_hdl, &bulk_sgnum);
	if (rc != 0) {
		C_ERROR("crt_bulk_get_sgnum failed, rc: %d.\n", rc);
		C_GOTO(out, rc);
	}
	rc = crt_bulk_get_len(bulk_hdl, &bulk_len);
	if (rc != 0) {
		C_ERROR("crt_bulk_get_len failed, rc: %d.\n", rc);
		C_GOTO(out, rc);
	}

	if (sgl->sg_nr.num < bulk_sgnum) {
		C_DEBUG("sgl->sg_nr.num (%d) too small, %d required.\n",
			sgl->sg_nr.num, bulk_sgnum);
		sgl->sg_nr.num_out = bulk_sgnum;
		C_GOTO(out, rc = -CER_TRUNC);
	}

	if (bulk_sgnum <= CRT_HG_IOVN_STACK) {
		allocate = 0;
		buf_sizes = buf_sizes_stack;
		buf_ptrs = buf_ptrs_stack;
	} else {
		allocate = 1;
		C_ALLOC(buf_sizes, bulk_sgnum * sizeof(hg_size_t));
		if (buf_sizes == NULL)
			C_GOTO(out, rc = -CER_NOMEM);

		C_ALLOC(buf_ptrs, bulk_sgnum * sizeof(void *));
		if (buf_sizes == NULL) {
			C_FREE(buf_sizes, bulk_sgnum * sizeof(hg_size_t));
			C_GOTO(out, rc = -CER_NOMEM);
		}
	}

	hg_bulk_hdl = bulk_hdl;
	hg_ret = HG_Bulk_access(hg_bulk_hdl, 0, bulk_len, HG_BULK_READWRITE,
				bulk_sgnum, buf_ptrs, buf_sizes, &actual_sgnum);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("HG_Bulk_access failed, hg_ret: %d.\n", hg_ret);
		C_GOTO(out, rc = -CER_HG);
	}
	C_ASSERT(actual_sgnum == bulk_sgnum);

	for (i = 0; i < bulk_sgnum; i++) {
		sgl->sg_iovs[i].iov_buf = buf_ptrs[i];
		sgl->sg_iovs[i].iov_buf_len = buf_sizes[i];
		sgl->sg_iovs[i].iov_len = buf_sizes[i];
	}
	sgl->sg_nr.num_out = bulk_sgnum;

out:
	if (allocate) {
		C_FREE(buf_sizes, bulk_sgnum * sizeof(hg_size_t));
		C_FREE(buf_ptrs, bulk_sgnum * sizeof(void *));
	}
	return rc;
}

struct crt_hg_bulk_cbinfo {
	struct crt_bulk_desc	*bci_desc;
	crt_bulk_cb_t		bci_cb;
	void			*bci_arg;
};

static hg_return_t
crt_hg_bulk_transfer_cb(const struct hg_cb_info *hg_cbinfo)
{
	struct crt_hg_bulk_cbinfo	*bulk_cbinfo;
	struct crt_bulk_cb_info		crt_bulk_cbinfo;
	struct crt_context		*ctx;
	struct crt_hg_context		*hg_ctx;
	struct crt_bulk_desc		*bulk_desc;
	hg_return_t			hg_ret = HG_SUCCESS;
	int				rc = 0;

	C_ASSERT(hg_cbinfo != NULL);
	bulk_cbinfo = (struct crt_hg_bulk_cbinfo *)hg_cbinfo->arg;
	C_ASSERT(bulk_cbinfo != NULL);
	bulk_desc = bulk_cbinfo->bci_desc;
	C_ASSERT(bulk_desc != NULL);
	ctx = (struct crt_context *)bulk_desc->bd_rpc->cr_ctx;
	hg_ctx = &ctx->cc_hg_ctx;
	C_ASSERT(hg_ctx != NULL);
	C_ASSERT(hg_cbinfo->type == HG_CB_BULK);
	C_ASSERT(hg_cbinfo->info.bulk.origin_handle ==
		 bulk_desc->bd_remote_hdl);
	C_ASSERT(hg_cbinfo->info.bulk.local_handle ==
		 bulk_desc->bd_local_hdl);

	if (hg_cbinfo->ret != HG_SUCCESS) {
		if (hg_cbinfo->ret == HG_CANCELED) {
			C_DEBUG("bulk transferring canceled.\n");
			rc = -CER_CANCELED;
		} else {
			C_ERROR("crt_hg_bulk_transfer_cb,hg_cbinfo->ret: %d.\n",
				hg_cbinfo->ret);
			hg_ret = hg_cbinfo->ret;
			rc = -CER_HG;
		}
	}

	if (bulk_cbinfo->bci_cb == NULL) {
		C_DEBUG("No bulk completion callback registered.\n");
		C_GOTO(out, hg_ret);
	}
	crt_bulk_cbinfo.bci_arg = bulk_cbinfo->bci_arg;
	crt_bulk_cbinfo.bci_rc = rc;
	crt_bulk_cbinfo.bci_bulk_desc = bulk_desc;

	rc = bulk_cbinfo->bci_cb(&crt_bulk_cbinfo);
	if (rc != 0)
		C_ERROR("bulk_cbinfo->bci_cb failed, rc: %d.\n", rc);

out:
	C_FREE_PTR(bulk_cbinfo);
	C_FREE_PTR(bulk_desc);
	return hg_ret;
}

int
crt_hg_bulk_transfer(struct crt_bulk_desc *bulk_desc, crt_bulk_cb_t complete_cb,
		     void *arg, crt_bulk_opid_t *opid)
{
	struct crt_context		*ctx;
	struct crt_hg_context		*hg_ctx;
	struct crt_hg_bulk_cbinfo	*bulk_cbinfo;
	hg_bulk_op_t			hg_bulk_op;
	struct crt_bulk_desc		*bulk_desc_dup;
	struct crt_rpc_priv		*rpc_priv;
	hg_return_t			hg_ret = HG_SUCCESS;
	int				rc = 0;

	C_ASSERT(bulk_desc != NULL);
	C_ASSERT(bulk_desc->bd_bulk_op == CRT_BULK_PUT ||
		 bulk_desc->bd_bulk_op == CRT_BULK_GET);
	C_ASSERT(bulk_desc->bd_rpc != NULL);
	ctx = (struct crt_context *)bulk_desc->bd_rpc->cr_ctx;
	hg_ctx = &ctx->cc_hg_ctx;
	C_ASSERT(hg_ctx != NULL && hg_ctx->chc_bulkctx != NULL);

	C_ALLOC_PTR(bulk_cbinfo);
	if (bulk_cbinfo == NULL)
		C_GOTO(out, rc = -CER_NOMEM);
	C_ALLOC_PTR(bulk_desc_dup);
	if (bulk_desc_dup == NULL) {
		C_FREE_PTR(bulk_cbinfo);
		C_GOTO(out, rc = -CER_NOMEM);
	}
	crt_bulk_desc_dup(bulk_desc_dup, bulk_desc);

	bulk_cbinfo->bci_desc = bulk_desc_dup;
	bulk_cbinfo->bci_cb = complete_cb;
	bulk_cbinfo->bci_arg = arg;

	hg_bulk_op = (bulk_desc->bd_bulk_op == CRT_BULK_PUT) ?
		     HG_BULK_PUSH : HG_BULK_PULL;
	rpc_priv = container_of(bulk_desc->bd_rpc, struct crt_rpc_priv,
				crp_pub);
	hg_ret = HG_Bulk_transfer(hg_ctx->chc_bulkctx, crt_hg_bulk_transfer_cb,
			bulk_cbinfo, hg_bulk_op, rpc_priv->crp_na_addr,
			bulk_desc->bd_remote_hdl, bulk_desc->bd_remote_off,
			bulk_desc->bd_local_hdl, bulk_desc->bd_local_off,
			bulk_desc->bd_len,
			opid != NULL ? (hg_op_id_t *)opid : HG_OP_ID_IGNORE);
	if (hg_ret != HG_SUCCESS) {
		C_ERROR("HG_Bulk_transfer failed, hg_ret: %d.\n", hg_ret);
		C_FREE_PTR(bulk_cbinfo);
		C_FREE_PTR(bulk_desc_dup);
		rc = -CER_HG;
	}

out:
	return rc;
}
