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
 * This file is part of CaRT. It gives out the generic tree topo related
 * function implementation.
 */

#include <crt_internal.h>

static int
crt_get_filtered_grp_rank_list(struct crt_grp_priv *grp_priv, uint32_t grp_ver,
			       crt_rank_list_t *exclude_ranks, crt_rank_t root,
			       crt_rank_t self, crt_rank_t *grp_size,
			       uint32_t *grp_root, crt_rank_t *grp_self,
			       crt_rank_list_t **result_grp_rank_list,
			       bool *allocated)
{
	crt_rank_list_t		*grp_rank_list = NULL;
	int			 rc = 0;

	if (exclude_ranks == NULL || exclude_ranks->rl_nr.num == 0) {
		grp_rank_list = grp_priv->gp_membs;
		*grp_size = grp_priv->gp_size;
		C_ASSERT(*grp_size == grp_rank_list->rl_nr.num);
		*grp_root = root;
		*grp_self = self;
		*allocated = false;
	} else {
		/* grp_priv->gp_membs/exclude_ranks already sorted and unique */
		rc = crt_rank_list_dup(&grp_rank_list, grp_priv->gp_membs,
				       true /* input */);
		if (rc != 0) {
			C_ERROR("crt_rank_list_dup failed, rc: %d.\n", rc);
			C_GOTO(out, rc);
		}
		C_ASSERT(grp_rank_list != NULL);
		crt_rank_list_filter(exclude_ranks, grp_rank_list,
				     true /* input */, true /* exclude */);

		if (grp_rank_list->rl_nr.num == 0) {
			C_DEBUG("crt_rank_list_filter(group %s) get empty.\n",
				grp_priv->gp_pub.cg_grpid);
			crt_rank_list_free(grp_rank_list);
			grp_rank_list = NULL;
			C_GOTO(out, rc = 0);
		}

		*allocated = true;
		*grp_size = grp_rank_list->rl_nr.num;
		rc = crt_idx_in_rank_list(grp_rank_list,
					  grp_priv->gp_membs->rl_ranks[root],
					  grp_root, true /* input */);
		if (rc != 0) {
			C_ERROR("crt_idx_in_rank_list (group %s, rank %d), "
				"failed, rc: %d.\n", grp_priv->gp_pub.cg_grpid,
				root, rc);
			C_GOTO(out, rc);
		}
		rc = crt_idx_in_rank_list(grp_rank_list,
					  grp_priv->gp_membs->rl_ranks[self],
					  grp_self, true /* input */);
		if (rc != 0) {
			C_ERROR("crt_idx_in_rank_list (group %s, rank %d), "
				"failed, rc: %d.\n", grp_priv->gp_pub.cg_grpid,
				self, rc);
			C_GOTO(out, rc);
		}
	}
out:
	if (rc == 0)
		*result_grp_rank_list = grp_rank_list;
	return rc;
}

/*
 * query children rank list (rank number in primary group).
 *
 * rank number of grp_priv->gp_membs and exclude_ranks are primary rank.
 * grp_root and grp_self are logical rank number within the group.
 */
int
crt_tree_get_children(struct crt_grp_priv *grp_priv, uint32_t grp_ver,
		      crt_rank_list_t *exclude_ranks, int tree_topo,
		      crt_rank_t root, crt_rank_t self,
		      crt_rank_list_t **children_rank_list)
{
	crt_rank_list_t		*grp_rank_list = NULL;
	crt_rank_list_t		*result_rank_list = NULL;
	crt_rank_t		 grp_root, grp_self;
	bool			 allocated = false;
	uint32_t		 tree_type, tree_ratio;
	uint32_t		 grp_size, nchildren;
	uint32_t		 *tree_children;
	struct crt_topo_ops	*tops;
	int			 i, rc = 0;

	C_ASSERT(grp_priv != NULL && grp_priv->gp_membs != NULL);
	C_ASSERT(root < grp_priv->gp_size && self < grp_priv->gp_size);
	C_ASSERT(crt_tree_topo_valid(tree_topo));
	tree_type = crt_tree_type(tree_topo);
	tree_ratio = crt_tree_ratio(tree_topo);
	C_ASSERT(tree_type >= CRT_TREE_MIN && tree_type <= CRT_TREE_MAX);
	C_ASSERT(tree_type == CRT_TREE_FLAT ||
		 (tree_ratio >= CRT_TREE_MIN_RATIO &&
		  tree_ratio <= CRT_TREE_MAX_RATIO));

	/*
	 * grp_rank_list is the target group (filtered out the excluded ranks)
	 * for building the tree, rank number in it is for primary group.
	 */
	rc = crt_get_filtered_grp_rank_list(grp_priv, grp_ver, exclude_ranks,
					    root, self, &grp_size, &grp_root,
					    &grp_self, &grp_rank_list,
					    &allocated);
	if (rc != 0) {
		C_ERROR("crt_get_filtered_grp_rank_list(group %s, root %d, "
			"self %d) failed, rc: %d.\n", grp_priv->gp_pub.cg_grpid,
			root, self, rc);
		C_GOTO(out, rc);
	}
	if (grp_rank_list == NULL) {
		C_DEBUG("crt_get_filtered_grp_rank_list(group %s) get empty.\n",
			grp_priv->gp_pub.cg_grpid);
		*children_rank_list = NULL;
		C_GOTO(out, rc);
	}

	tops = crt_tops[tree_type];
	rc = tops->to_get_children_cnt(grp_size, tree_ratio, grp_root, grp_self,
				       &nchildren);
	if (rc != 0) {
		C_ERROR("to_get_children_cnt (group %s, root %d, self %d) "
			"failed, rc: %d.\n", grp_priv->gp_pub.cg_grpid,
			root, self, rc);
		C_GOTO(out, rc);
	}
	if (nchildren == 0) {
		*children_rank_list = NULL;
		C_GOTO(out, rc = 0);
	}
	result_rank_list = crt_rank_list_alloc(nchildren);
	if (result_rank_list == NULL)
		C_GOTO(out, rc = -CER_NOMEM);
	C_ALLOC(tree_children, nchildren * sizeof(uint32_t));
	if (tree_children == NULL) {
		crt_rank_list_free(result_rank_list);
		C_GOTO(out, rc = -CER_NOMEM);
	}
	rc = tops->to_get_children(grp_size, tree_ratio, grp_root, grp_self,
				   tree_children);
	if (rc != 0) {
		C_ERROR("to_get_children (group %s, root %d, self %d) "
			"failed, rc: %d.\n", grp_priv->gp_pub.cg_grpid,
			root, self, rc);
		crt_rank_list_free(result_rank_list);
		C_FREE(tree_children, nchildren * sizeof(uint32_t));
		C_GOTO(out, rc);
	}

	for (i = 0; i < nchildren; i++)
		result_rank_list->rl_ranks[i] =
			grp_rank_list->rl_ranks[tree_children[i]];

	*children_rank_list = result_rank_list;

out:
	if (allocated)
		crt_rank_list_free(grp_rank_list);
	return rc;
}

int
crt_tree_get_parent(struct crt_grp_priv *grp_priv, uint32_t grp_ver,
		    crt_rank_list_t *exclude_ranks, int tree_topo,
		    crt_rank_t root, crt_rank_t self, crt_rank_t *parent_rank)
{
	crt_rank_list_t		*grp_rank_list = NULL;
	crt_rank_t		 grp_root, grp_self;
	bool			 allocated = false;
	uint32_t		 tree_type, tree_ratio;
	uint32_t		 grp_size, tree_parent;
	struct crt_topo_ops	*tops;
	int			 rc = 0;

	C_ASSERT(grp_priv != NULL && grp_priv->gp_membs != NULL);
	C_ASSERT(root < grp_priv->gp_size && self < grp_priv->gp_size);
	C_ASSERT(crt_tree_topo_valid(tree_topo));
	C_ASSERT(parent_rank != NULL);
	tree_type = crt_tree_type(tree_topo);
	tree_ratio = crt_tree_ratio(tree_topo);
	C_ASSERT(tree_type >= CRT_TREE_MIN && tree_type <= CRT_TREE_MAX);
	C_ASSERT(tree_type == CRT_TREE_FLAT ||
		 (tree_ratio >= CRT_TREE_MIN_RATIO &&
		  tree_ratio <= CRT_TREE_MAX_RATIO));

	/*
	 * grp_rank_list is the target group (filtered out the excluded ranks)
	 * for building the tree, rank number in it is for primary group.
	 */
	rc = crt_get_filtered_grp_rank_list(grp_priv, grp_ver, exclude_ranks,
					    root, self, &grp_size, &grp_root,
					    &grp_self, &grp_rank_list,
					    &allocated);
	if (rc != 0) {
		C_ERROR("crt_get_filtered_grp_rank_list(group %s, root %d, "
			"self %d) failed, rc: %d.\n", grp_priv->gp_pub.cg_grpid,
			root, self, rc);
		C_GOTO(out, rc);
	}
	if (grp_rank_list == NULL) {
		C_DEBUG("crt_get_filtered_grp_rank_list(group %s) get empty.\n",
			grp_priv->gp_pub.cg_grpid);
		C_GOTO(out, rc = -CER_INVAL);
	}

	tops = crt_tops[tree_type];
	rc = tops->to_get_parent(grp_size, tree_ratio, grp_root, grp_self,
				 &tree_parent);
	if (rc != 0) {
		C_ERROR("to_get_parent (group %s, root %d, self %d) failed, "
			"rc: %d.\n", grp_priv->gp_pub.cg_grpid, root, self, rc);
	}

	*parent_rank = grp_rank_list->rl_ranks[tree_parent];

out:
	if (allocated)
		crt_rank_list_free(grp_rank_list);
	return rc;
}

struct crt_topo_ops *crt_tops[] = {
	NULL,			/* CRT_TREE_INVALID */
	&crt_flat_ops,		/* CRT_TREE_FLAT */
	&crt_kary_ops,		/* CRT_TREE_KARY */
	&crt_knomial_ops,	/* CRT_TREE_KNOMIAL */
};
