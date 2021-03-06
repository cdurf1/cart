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
 *
 * This file is part of CaRT. It implements the main RPC routines.
 */

#include <crt_internal.h>

/* CRT internal RPC format definitions */

/* group create */
static struct crt_msg_field *crt_grp_create_in_fields[] = {
	&CMF_GRP_ID,		/* gc_grp_id */
	&CMF_UINT64,		/* gc_int_grpid */
	&CMF_RANK_LIST,		/* gc_membs */
	&CMF_RANK,		/* gc_initiate_rank */
};

static struct crt_msg_field *crt_grp_create_out_fields[] = {
	&CMF_RANK_LIST,		/* gc_failed_ranks */
	&CMF_RANK,		/* gc_rank */
	&CMF_INT,		/* gc_rc */
};

static struct crt_req_format CQF_CRT_GRP_CREATE =
	DEFINE_CRT_REQ_FMT("CRT_GRP_CREATE", crt_grp_create_in_fields,
			   crt_grp_create_out_fields);

/* group destroy */
static struct crt_msg_field *crt_grp_destroy_in_fields[] = {
	&CMF_GRP_ID,		/* gd_grp_id */
	&CMF_RANK,		/* gd_initiate_rank */
};

static struct crt_msg_field *crt_grp_destroy_out_fields[] = {
	&CMF_RANK_LIST,		/* gd_failed_ranks */
	&CMF_RANK,		/* gd_rank */
	&CMF_INT,		/* gd_rc */
};

static struct crt_req_format CQF_CRT_GRP_DESTROY =
	DEFINE_CRT_REQ_FMT("CRT_GRP_DESTROY", crt_grp_destroy_in_fields,
			   crt_grp_destroy_out_fields);

/* uri lookup */
static struct crt_msg_field *crt_uri_lookup_in_fields[] = {
	&CMF_GRP_ID,		/* ul_grp_id */
	&CMF_RANK,		/* ul_rank */
};

static struct crt_msg_field *crt_uri_lookup_out_fields[] = {
	&CMF_PHY_ADDR,		/* ul_uri */
	&CMF_INT,		/* ul_rc */
};

static struct crt_req_format CQF_CRT_URI_LOOKUP =
	DEFINE_CRT_REQ_FMT("CRT_URI_LOOKUP", crt_uri_lookup_in_fields,
			   crt_uri_lookup_out_fields);

struct crt_internal_rpc crt_internal_rpcs[] = {
	{
		.ir_name	= "CRT_GRP_CREATE",
		.ir_opc		= CRT_OPC_GRP_CREATE,
		.ir_ver		= 1,
		.ir_flags	= 0,
		.ir_req_fmt	= &CQF_CRT_GRP_CREATE,
		.ir_hdlr	= crt_hdlr_grp_create,
		.ir_co_ops	= NULL,
	}, {
		.ir_name	= "CRT_GRP_DESTROY",
		.ir_opc		= CRT_OPC_GRP_DESTROY,
		.ir_ver		= 1,
		.ir_flags	= 0,
		.ir_req_fmt	= &CQF_CRT_GRP_DESTROY,
		.ir_hdlr	= crt_hdlr_grp_destroy,
		.ir_co_ops	= NULL,
	}, {
		.ir_name	= "CRT_URI_LOOKUP",
		.ir_opc		= CRT_OPC_URI_LOOKUP,
		.ir_ver		= 1,
		.ir_flags	= 0,
		.ir_req_fmt	= &CQF_CRT_URI_LOOKUP,
		.ir_hdlr	= crt_hdlr_uri_lookup,
		.ir_co_ops	= NULL,
	}, {
		.ir_opc		= 0
	}
};

/* CRT RPC related APIs or internal functions */

int
crt_internal_rpc_register(void)
{
	struct crt_internal_rpc *rpc;
	int	rc = 0;

	/* walk through the handler list and register each individual RPC */
	for (rpc = crt_internal_rpcs; rpc->ir_opc != 0; rpc++) {
		C_ASSERT(rpc->ir_hdlr != NULL);
		rc = crt_rpc_reg_internal(rpc->ir_opc, rpc->ir_req_fmt,
					   rpc->ir_hdlr, rpc->ir_co_ops);
		if (rc) {
			C_ERROR("opcode 0x%x registration failed, rc: %d.\n",
				rpc->ir_opc, rc);
			break;
		}
	}
	return rc;
}

int
crt_rpc_priv_alloc(crt_opcode_t opc, struct crt_rpc_priv **priv_allocated)
{
	struct crt_rpc_priv	*rpc_priv;
	struct crt_opc_info	*opc_info;
	int			rc = 0;

	C_ASSERT(priv_allocated != NULL);

	opc_info = crt_opc_lookup(crt_gdata.cg_opc_map, opc, CRT_UNLOCK);
	if (opc_info == NULL) {
		C_ERROR("opc: 0x%x, lookup failed.\n", opc);
		C_GOTO(out, rc = -CER_UNREG);
	}
	C_ASSERT(opc_info->coi_input_size <= CRT_MAX_INPUT_SIZE &&
		 opc_info->coi_output_size <= CRT_MAX_OUTPUT_SIZE);

	C_ALLOC_PTR(rpc_priv);
	if (rpc_priv == NULL)
		C_GOTO(out, rc = -CER_NOMEM);

	rpc_priv->crp_opc_info = opc_info;
	*priv_allocated = rpc_priv;

out:
	return rc;
}

void
crt_rpc_priv_free(struct crt_rpc_priv *rpc_priv)
{
	if (rpc_priv == NULL)
		return;

	if (rpc_priv->crp_coll && rpc_priv->crp_corpc_info) {
		crt_rank_list_free(
			rpc_priv->crp_corpc_info->co_excluded_ranks);
		C_FREE_PTR(rpc_priv->crp_corpc_info);
	}

	pthread_spin_destroy(&rpc_priv->crp_lock);
	C_FREE_PTR(rpc_priv);
}

int
crt_req_create_internal(crt_context_t crt_ctx, crt_endpoint_t tgt_ep,
			crt_opcode_t opc, bool forward, crt_rpc_t **req)
{
	struct crt_context	*ctx;
	struct crt_rpc_priv	*rpc_priv = NULL;
	crt_rpc_t		*rpc_pub;
	int			 rc = 0;

	C_ASSERT(crt_ctx != CRT_CONTEXT_NULL && req != NULL);
	ctx = (struct crt_context *)crt_ctx;

	rc = crt_rpc_priv_alloc(opc, &rpc_priv);
	if (rc != 0) {
		C_ERROR("crt_rpc_priv_alloc, rc: %d, opc: 0x%x.\n", rc, opc);
		C_GOTO(out, rc);
	}
	C_ASSERT(rpc_priv != NULL);
	rpc_pub = &rpc_priv->crp_pub;
	rpc_pub->cr_ep = tgt_ep;

	rc = crt_rpc_priv_init(rpc_priv, crt_ctx, opc, false /* srv_flag */,
			       forward);
	if (rc != 0) {
		C_ERROR("crt_rpc_priv_init failed, opc: 0x%x, rc: %d.\n",
			opc, rc);
		C_GOTO(out, rc);
	}

	rc = crt_hg_req_create(&ctx->cc_hg_ctx, ctx->cc_idx, tgt_ep, rpc_priv);
	if (rc != 0) {
		C_ERROR("crt_hg_req_create failed, rc: %d, opc: 0x%x.\n",
			rc, opc);
		C_GOTO(out, rc);
	}

	*req = rpc_pub;

out:
	if (rc < 0)
		crt_rpc_priv_free(rpc_priv);
	return rc;
}

int
crt_req_create(crt_context_t crt_ctx, crt_endpoint_t tgt_ep, crt_opcode_t opc,
	       crt_rpc_t **req)
{
	struct crt_grp_gdata	*grp_gdata;
	struct crt_grp_priv	*grp_priv;
	int			 rc = 0;

	if (crt_ctx == CRT_CONTEXT_NULL || req == NULL) {
		C_ERROR("invalid parameter (NULL crt_ctx or req).\n");
		C_GOTO(out, rc = -CER_INVAL);
	}
	if (!crt_initialized()) {
		C_ERROR("CRT not initialized.\n");
		C_GOTO(out, rc = -CER_UNINIT);
	}
	grp_gdata = crt_gdata.cg_grp;
	C_ASSERT(grp_gdata != NULL);
	if (tgt_ep.ep_grp == NULL) {
		grp_priv = grp_gdata->gg_srv_pri_grp;
	} else {
		grp_priv = container_of(tgt_ep.ep_grp, struct crt_grp_priv,
					gp_pub);
		if (grp_priv->gp_primary == 0 || grp_priv->gp_service == 0) {
			C_ERROR("bad parameter tgt_ep.ep_grp: %p (gp_primary: "
				"%d, gp_service: %d, gp_local: %d.\n",
				tgt_ep.ep_grp, grp_priv->gp_primary,
				grp_priv->gp_service, grp_priv->gp_local);
			C_GOTO(out, rc = -CER_INVAL);
		}
	}
	if (tgt_ep.ep_rank >= grp_priv->gp_size) {
		C_ERROR("invalid parameter, rank %d, group_size: %d.\n",
			tgt_ep.ep_rank, grp_priv->gp_size);
		C_GOTO(out, rc = -CER_INVAL);
	}

	rc = crt_req_create_internal(crt_ctx, tgt_ep, opc, false /* forward */,
				     req);
	if (rc != 0) {
		C_ERROR("crt_req_create_internal failed, opc: 0x%x, rc: %d.\n",
			opc, rc);
		C_GOTO(out, rc);
	}
	C_ASSERT(*req != NULL);

out:
	return rc;
}

int
crt_req_addref(crt_rpc_t *req)
{
	struct crt_rpc_priv	*rpc_priv = NULL;
	int			rc = 0;

	if (req == NULL) {
		C_ERROR("invalid parameter (NULL req).\n");
		C_GOTO(out, rc = -CER_INVAL);
	}

	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);
	pthread_spin_lock(&rpc_priv->crp_lock);
	rpc_priv->crp_refcount++;
	pthread_spin_unlock(&rpc_priv->crp_lock);

out:
	return rc;
}

int
crt_req_decref(crt_rpc_t *req)
{
	struct crt_rpc_priv	*rpc_priv = NULL;
	int			rc = 0, destroy = 0;

	if (req == NULL) {
		C_ERROR("invalid parameter (NULL req).\n");
		C_GOTO(out, rc = -CER_INVAL);
	}

	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);
	pthread_spin_lock(&rpc_priv->crp_lock);
	rpc_priv->crp_refcount--;
	if (rpc_priv->crp_refcount == 0)
		destroy = 1;
	pthread_spin_unlock(&rpc_priv->crp_lock);

	if (destroy == 1) {
		rc = crt_hg_req_destroy(rpc_priv);
		if (rc != 0)
			C_ERROR("crt_hg_req_destroy failed, rc: %d, "
				"opc: 0x%x.\n", rc, req->cr_opc);
	}

out:
	return rc;
}

int
crt_req_send(crt_rpc_t *req, crt_cb_t complete_cb, void *arg)
{
	struct crt_rpc_priv	*rpc_priv = NULL;
	int			rc = 0;

	if (req == NULL) {
		C_ERROR("invalid parameter (NULL req).\n");
		C_GOTO(out, rc = -CER_INVAL);
	}
	if (req->cr_ctx == NULL) {
		C_ERROR("invalid parameter (NULL req->cr_ctx).\n");
		C_GOTO(out, rc = -CER_INVAL);
	}

	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);
	rpc_priv->crp_complete_cb = complete_cb;
	rpc_priv->crp_arg = arg;

	if (rpc_priv->crp_coll) {
		rc = crt_corpc_req_hdlr(req);
		if (rc != 0)
			C_ERROR("crt_corpc_req_hdlr failed,rc: %d,opc: 0x%x.\n",
				rc, req->cr_opc);
		C_GOTO(out, rc);
	}

	rc = crt_context_req_track(req);
	if (rc == CRT_REQ_TRACK_IN_INFLIGHQ) {
		/* tracked in crt_ep_inflight::epi_req_q */
		/* set state before sending to avoid race with complete_cb */
		rpc_priv->crp_state = RPC_REQ_SENT;
		rc = crt_hg_req_send(rpc_priv);
		if (rc != 0) {
			C_ERROR("crt_hg_req_send failed, rc: %d, opc: 0x%x.\n",
				rc, rpc_priv->crp_pub.cr_opc);
			rpc_priv->crp_state = RPC_INITED;
			crt_context_req_untrack(req);
		}
	} else if (rc == CRT_REQ_TRACK_IN_WAITQ) {
		/* queued in crt_hg_context::dhc_req_q */
		rc = 0;
	} else {
		C_ERROR("crt_req_track failed, rc: %d, opc: 0x%x.\n",
			rc, rpc_priv->crp_pub.cr_opc);
	}

out:
	/* internally destroy the req when failed */
	if (rc != 0 && req != NULL)
		crt_req_decref(req);
	return rc;
}

int
crt_reply_send(crt_rpc_t *req)
{
	struct crt_rpc_priv	*rpc_priv = NULL;
	int			rc = 0;

	if (req == NULL) {
		C_ERROR("invalid parameter (NULL req).\n");
		C_GOTO(out, rc = -CER_INVAL);
	}

	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);

	if (rpc_priv->crp_coll == 1) {
		struct crt_cb_info	cb_info;

		cb_info.cci_rpc = &rpc_priv->crp_pub;
		cb_info.cci_rc = 0;
		cb_info.cci_arg = rpc_priv;

		rc = crt_corpc_reply_hdlr(&cb_info);
		if (rc != 0)
			C_ERROR("crt_corpc_reply_hdlr failed, rc: %d, "
				"opc: 0x%x.\n", rc, rpc_priv->crp_pub.cr_opc);
	} else {
		rc = crt_hg_reply_send(rpc_priv);
		if (rc != 0)
			C_ERROR("crt_hg_reply_send failed, rc: %d,opc: 0x%x.\n",
				rc, rpc_priv->crp_pub.cr_opc);
	}

out:
	return rc;
}

int
crt_req_abort(crt_rpc_t *req)
{
	struct crt_rpc_priv	*rpc_priv = NULL;
	int			rc = 0;

	if (req == NULL) {
		C_ERROR("invalid parameter (NULL req).\n");
		C_GOTO(out, rc = -CER_INVAL);
	}

	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);
	rc = crt_hg_req_cancel(rpc_priv);
	if (rc != 0) {
		C_ERROR("crt_hg_req_cancel failed, rc: %d, opc: 0x%x.\n",
			rc, rpc_priv->crp_pub.cr_opc);
	}

out:
	return rc;
}

static int
crt_cb_common(const struct crt_cb_info *cb_info)
{
	*(int *)cb_info->cci_arg = 1;
	return 0;
}

/**
 * Send rpc synchronously
 *
 * \param[IN] rpc	point to CRT request.
 * \param[IN] timeout	timeout (Micro-seconds) to wait, if
 *                      timeout <= 0, it will wait infinitely.
 * \return		0 if rpc return successfuly.
 * \return		negative errno if sending fails or timeout.
 */
int
crt_req_send_sync(crt_rpc_t *rpc, uint64_t timeout)
{
	uint64_t now;
	uint64_t end;
	int rc;
	int complete = 0;

	/* Send request */
	rc = crt_req_send(rpc, crt_cb_common, &complete);
	if (rc != 0)
		return rc;

	/* Check if we are lucky */
	if (complete)
		return 0;

	timeout = timeout ? timeout : CRT_DEFAULT_TIMEOUT_US;
	/* Wait the request to be completed in timeout milliseconds */
	end = crt_timeus_secdiff(0) + timeout;

	while (1) {
		uint64_t interval = 1000; /* microseconds */

		rc = crt_progress(rpc->cr_ctx, interval, NULL, NULL);
		if (rc != 0 && rc != -CER_TIMEDOUT) {
			C_ERROR("crt_progress failed rc: %d.\n", rc);
			break;
		}

		if (complete) {
			rc = 0;
			break;
		}

		now = crt_timeus_secdiff(0);
		if (now >= end) {
			rc = -CER_TIMEDOUT;
			break;
		}
	}

	return rc;
}

static void
crt_rpc_inout_buff_fini(struct crt_rpc_priv *rpc_priv)
{
	crt_rpc_t	*rpc_pub;

	C_ASSERT(rpc_priv != NULL);
	rpc_pub = &rpc_priv->crp_pub;

	if (rpc_pub->cr_input != NULL) {
		C_ASSERT(rpc_pub->cr_input_size != 0);
		if (!rpc_priv->crp_forward)
			C_FREE(rpc_pub->cr_input, rpc_pub->cr_input_size);
		rpc_pub->cr_input_size = 0;
	}

	if (rpc_pub->cr_output != NULL) {
		C_ASSERT(rpc_pub->cr_output_size != 0);
		C_FREE(rpc_pub->cr_output, rpc_pub->cr_output_size);
		rpc_pub->cr_output_size = 0;
	}
}

static int
crt_rpc_inout_buff_init(struct crt_rpc_priv *rpc_priv)
{
	crt_rpc_t		*rpc_pub;
	struct crt_opc_info	*opc_info;
	int			rc = 0;

	C_ASSERT(rpc_priv != NULL);
	rpc_pub = &rpc_priv->crp_pub;
	C_ASSERT(rpc_pub->cr_input == NULL);
	C_ASSERT(rpc_pub->cr_output == NULL);
	rpc_priv = container_of(rpc_pub, struct crt_rpc_priv, crp_pub);
	opc_info = rpc_priv->crp_opc_info;
	C_ASSERT(opc_info != NULL);

	/*
	 * for forward request, need not allocate memory here, instead it will
	 * reuse the original input buffer of parent RPC.
	 * See crt_corpc_req_hdlr().
	 */
	if (opc_info->coi_input_size > 0 && !rpc_priv->crp_forward) {
		C_ALLOC(rpc_pub->cr_input, opc_info->coi_input_size);
		if (rpc_pub->cr_input == NULL) {
			C_ERROR("cannot allocate memory(size "CF_U64") for "
				"cr_input.\n", opc_info->coi_input_size);
			C_GOTO(out, rc = -CER_NOMEM);
		}
		rpc_pub->cr_input_size = opc_info->coi_input_size;
	}
	if (opc_info->coi_output_size > 0) {
		C_ALLOC(rpc_pub->cr_output, opc_info->coi_output_size);
		if (rpc_pub->cr_output == NULL) {
			C_ERROR("cannot allocate memory(size "CF_U64") for "
				"cr_putput.\n", opc_info->coi_input_size);
			C_GOTO(out, rc = -CER_NOMEM);
		}
		rpc_pub->cr_output_size = opc_info->coi_output_size;
	}

out:
	if (rc < 0)
		crt_rpc_inout_buff_fini(rpc_priv);
	return rc;
}

int
crt_rpc_priv_init(struct crt_rpc_priv *rpc_priv, crt_context_t crt_ctx,
		  crt_opcode_t opc, bool srv_flag, bool forward)
{
	int	rc = 0;

	C_ASSERT(rpc_priv != NULL);
	CRT_INIT_LIST_HEAD(&rpc_priv->crp_epi_link);
	CRT_INIT_LIST_HEAD(&rpc_priv->crp_tmp_link);
	CRT_INIT_LIST_HEAD(&rpc_priv->crp_parent_link);
	rpc_priv->crp_complete_cb = NULL;
	rpc_priv->crp_arg = NULL;
	if (!srv_flag) {
		crt_common_hdr_init(&rpc_priv->crp_req_hdr, opc);
		crt_common_hdr_init(&rpc_priv->crp_reply_hdr, opc);
	}
	rpc_priv->crp_state = RPC_INITED;
	rpc_priv->crp_srv = srv_flag;
	rpc_priv->crp_forward = forward;
	/* initialize as 1, so user can cal crt_req_decref to destroy new req */
	rpc_priv->crp_refcount = 1;
	pthread_spin_init(&rpc_priv->crp_lock, PTHREAD_PROCESS_PRIVATE);

	rpc_priv->crp_pub.cr_opc = opc;
	rpc_priv->crp_pub.cr_ctx = crt_ctx;

	rc = crt_rpc_inout_buff_init(rpc_priv);
	if (rc != 0)
		C_ERROR("crt_rpc_inout_buff_init failed, opc: 0x%x, rc: %d.\n",
			rpc_priv->crp_pub.cr_opc, rc);

	return rc;
}

void
crt_rpc_priv_fini(struct crt_rpc_priv *rpc_priv)
{
	C_ASSERT(rpc_priv != NULL);
	crt_rpc_inout_buff_fini(rpc_priv);
}

static void
crt_handle_rpc(void *arg)
{
	struct crt_rpc_priv	*rpc_priv = arg;
	crt_rpc_t		*rpc_pub;

	C_ASSERT(rpc_priv != NULL);
	C_ASSERT(rpc_priv->crp_opc_info != NULL);
	C_ASSERT(rpc_priv->crp_opc_info->coi_rpc_cb != NULL);
	rpc_pub = &rpc_priv->crp_pub;
	rpc_priv->crp_opc_info->coi_rpc_cb(rpc_pub);
	crt_req_decref(rpc_pub);
}

int
crt_rpc_common_hdlr(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context	*crt_ctx;
	int			 rc = 0;

	C_ASSERT(rpc_priv != NULL);
	crt_ctx = (struct crt_context *)rpc_priv->crp_pub.cr_ctx;

	if (crt_ctx->cc_pool != NULL) {
		rc = ABT_thread_create(*(ABT_pool *)crt_ctx->cc_pool,
				       crt_handle_rpc, rpc_priv,
				       ABT_THREAD_ATTR_NULL, NULL);
	} else {
		rc = rpc_priv->crp_opc_info->coi_rpc_cb(&rpc_priv->crp_pub);
		if (rc != 0)
			C_ERROR("coi_rpc_cb failed, rc: %d, opc: 0x%x.\n",
				rc, rpc_priv->crp_pub.cr_opc);
	}

	return rc;
}

static int
timeout_bp_node_enter(struct crt_binheap *h, struct crt_binheap_node *e)
{
	struct crt_rpc_priv	*rpc_priv;

	C_ASSERT(h != NULL);
	C_ASSERT(e != NULL);

	rpc_priv = container_of(e, struct crt_rpc_priv, crp_timeout_bp_node);

	C_DEBUG("rpc_priv %p (opc 0x%x) entering the timeout binheap.\n",
		rpc_priv, rpc_priv->crp_pub.cr_opc);

	return 0;
}

static int
timeout_bp_node_exit(struct crt_binheap *h, struct crt_binheap_node *e)
{
	struct crt_rpc_priv	*rpc_priv;

	C_ASSERT(h != NULL);
	C_ASSERT(e != NULL);

	rpc_priv = container_of(e, struct crt_rpc_priv, crp_timeout_bp_node);

	C_DEBUG("rpc_priv %p (opc 0x%x) exiting the timeout binheap.\n",
		rpc_priv, rpc_priv->crp_pub.cr_opc);

	return 0;
}

static bool
timeout_bp_node_cmp(struct crt_binheap_node *a, struct crt_binheap_node *b)
{
	struct crt_rpc_priv	*rpc_priv_a;
	struct crt_rpc_priv	*rpc_priv_b;

	C_ASSERT(a != NULL);
	C_ASSERT(b != NULL);

	rpc_priv_a = container_of(a, struct crt_rpc_priv, crp_timeout_bp_node);
	rpc_priv_b = container_of(b, struct crt_rpc_priv, crp_timeout_bp_node);

	return rpc_priv_a->crp_timeout_ts < rpc_priv_b->crp_timeout_ts;
}

struct crt_binheap_ops crt_timeout_bh_ops = {
	.hop_enter	= timeout_bp_node_enter,
	.hop_exit	= timeout_bp_node_exit,
	.hop_compare	= timeout_bp_node_cmp
};
