/*
 * xpu_common.c
 *
 * Core implementation of xPU device code
 * ----
 * Copyright 2011-2022 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2022 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include "xpu_common.h"

/*
 * Const Expression
 */
STATIC_FUNCTION(bool)
pgfn_ConstExpr(XPU_PGFUNCTION_ARGS)
{
	const void *addr;

	if (kexp->u.c.const_isnull)
		addr = NULL;
	else
		addr = kexp->u.c.const_value;
	return kexp->exptype_ops->xpu_datum_ref(kcxt, __result, NULL, addr, -1);
}

STATIC_FUNCTION(bool)
pgfn_ParamExpr(XPU_PGFUNCTION_ARGS)
{
	kern_session_info *session = kcxt->session;
	uint32_t	param_id = kexp->u.p.param_id;
	void	   *addr = NULL;

	if (param_id < session->nparams && session->poffset[param_id] != 0)
		addr = (char *)session + session->poffset[param_id];
	return kexp->exptype_ops->xpu_datum_ref(kcxt, __result, NULL, addr, -1);
}

STATIC_FUNCTION(bool)
pgfn_VarExpr(XPU_PGFUNCTION_ARGS)
{
	uint32_t	slot_id = kexp->u.v.var_slot_id;

	if (slot_id < kcxt->kvars_nslots)
	{
		return kexp->exptype_ops->xpu_datum_ref(kcxt, __result,
												kcxt->kvars_cmeta[slot_id],
												kcxt->kvars_addr[slot_id],
												kcxt->kvars_len[slot_id]);
	}
	STROM_ELOG(kcxt, "Bug? slot_id is out of range");
	return false;
}

STATIC_FUNCTION(bool)
pgfn_BoolExprAnd(XPU_PGFUNCTION_ARGS)
{
	xpu_bool_t *result = (xpu_bool_t *)__result;
	int			i;
	bool		anynull = false;
	const kern_expression *karg = KEXP_FIRST_ARG(-1,bool);

	memset(result, 0, sizeof(xpu_bool_t));
	result->ops = &xpu_bool_ops;
	for (i=0, karg=(const kern_expression *)kexp->u.data;
		 i < kexp->nargs;
		 i++, karg=(const kern_expression *)((char *)karg + MAXALIGN(VARSIZE(karg))))
	{
		xpu_bool_t	status;

		assert((char *)karg + VARSIZE(karg) <= (char *)kexp + VARSIZE(kexp));
		if (!EXEC_KERN_EXPRESSION(kcxt, karg, &status))
			return false;
		if (status.isnull)
			anynull = true;
		else if (!status.value)
		{
			result->value = false;
			return true;
		}
	}
	result->isnull = anynull;
	result->value  = true;
	return true;
}

STATIC_FUNCTION(bool)
pgfn_BoolExprOr(XPU_PGFUNCTION_ARGS)
{
	xpu_bool_t *result = (xpu_bool_t *)__result;
	int			i;
	bool		anynull = false;
	const kern_expression *arg = KEXP_FIRST_ARG(-1,bool);

	result->ops = &xpu_bool_ops;
	for (i=0; i < kexp->nargs; i++)
	{
		xpu_bool_t	status;

		if (!EXEC_KERN_EXPRESSION(kcxt, arg, &status))
			return false;
		if (status.isnull)
			anynull = true;
		else if (status.value)
		{
			result->value = true;
			return true;
		}
		arg = KEXP_NEXT_ARG(arg, bool);
	}
	result->isnull = anynull;
	result->value  = false;
	return true;
}

STATIC_FUNCTION(bool)
pgfn_BoolExprNot(XPU_PGFUNCTION_ARGS)
{
	xpu_bool_t *result = (xpu_bool_t *)__result;
	xpu_bool_t	status;
	const kern_expression *arg = KEXP_FIRST_ARG(1,bool);

	if (!EXEC_KERN_EXPRESSION(kcxt, arg, &status))
		return false;
	result->ops = &xpu_bool_ops;
	if (status.isnull)
		result->isnull = true;
	else
	{
		result->isnull = false;
		result->value = !result->value;
	}
	return true;
}

STATIC_FUNCTION(bool)
pgfn_NullTestExpr(XPU_PGFUNCTION_ARGS)
{
	xpu_bool_t	   *result = (xpu_bool_t *)__result;
	xpu_datum_t	   *status;
	const kern_expression *arg = KEXP_FIRST_ARG(1,Invalid);

	status = (xpu_datum_t *)alloca(arg->exptype_ops->xpu_type_sizeof);
	if (!EXEC_KERN_EXPRESSION(kcxt, arg, status))
		return false;
	result->ops = &xpu_bool_ops;
	result->isnull = false;
	switch (kexp->opcode)
	{
		case FuncOpCode__NullTestExpr_IsNull:
			result->value = status->isnull;
			break;
		case FuncOpCode__NullTestExpr_IsNotNull:
			result->value = !status->isnull;
			break;
		default:
			STROM_ELOG(kcxt, "corrupted kernel expression");
			return false;
	}
	return true;
}

STATIC_FUNCTION(bool)
pgfn_BoolTestExpr(XPU_PGFUNCTION_ARGS)
{
	xpu_bool_t	   *result = (xpu_bool_t *)__result;
	xpu_bool_t		status;
	const kern_expression *arg = KEXP_FIRST_ARG(1,bool);

	if (!EXEC_KERN_EXPRESSION(kcxt, arg, &status))
		return false;
	result->ops = &xpu_bool_ops;
	result->isnull = false;
	switch (kexp->opcode)
	{
		case FuncOpCode__BoolTestExpr_IsTrue:
			result->value = (!status.isnull && status.value);
			break;
		case FuncOpCode__BoolTestExpr_IsNotTrue:
			result->value = (status.isnull || !status.value);
			break;
		case FuncOpCode__BoolTestExpr_IsFalse:
			result->value = (!status.isnull && !status.value);
			break;
		case FuncOpCode__BoolTestExpr_IsNotFalse:
			result->value = (status.isnull || status.value);
			break;
		case FuncOpCode__BoolTestExpr_IsUnknown:
			result->value = status.isnull;
			break;
		case FuncOpCode__BoolTestExpr_IsNotUnknown:
			result->value = !status.isnull;
			break;
		default:
			STROM_ELOG(kcxt, "corrupted kernel expression");
			return false;
	}
	return true;
}

/* ----------------------------------------------------------------
 *
 * Routines to support Projection
 *
 * ----------------------------------------------------------------
 */
PUBLIC_FUNCTION(int)
kern_form_heaptuple(kern_context *kcxt,
					const kern_expression *kproj,
					const kern_data_store *kds_dst,
					HeapTupleHeaderData *htup)
{
	const kern_projection_map *proj_map = __KEXP_GET_PROJECTION_MAP(kproj);
	uint32_t	t_hoff;
	uint32_t	t_next;
	uint16_t	t_infomask = 0;
	bool		t_hasnull = false;
	int			j, sz;

	/* has any NULL attributes? */
	for (j=0; j < proj_map->nattrs; j++)
	{
		const kern_projection_desc *desc = &proj_map->desc[proj_map->nexprs + j];

		assert(desc->slot_id < kcxt->kvars_nslots);
		if (!kcxt->kvars_addr[desc->slot_id])
		{
			t_infomask |= HEAP_HASNULL;
			t_hasnull = true;
			break;
		}
	}

	/* set up headers */
	t_hoff = offsetof(HeapTupleHeaderData, t_bits);
	if (t_hasnull)
		t_hoff += BITMAPLEN(proj_map->nattrs);
	t_hoff = MAXALIGN(t_hoff);

	if (htup)
	{
		memset(htup, 0, t_hoff);
		htup->t_choice.t_datum.datum_typmod = kds_dst->tdtypmod;
		htup->t_choice.t_datum.datum_typeid = kds_dst->tdtypeid;
		htup->t_ctid.ip_blkid.bi_hi = 0xffff;	/* InvalidBlockNumber */
		htup->t_ctid.ip_blkid.bi_lo = 0xffff;
		htup->t_ctid.ip_posid = 0;				/* InvalidOffsetNumber */
		htup->t_infomask2 = (proj_map->nattrs & HEAP_NATTS_MASK);
		htup->t_hoff = t_hoff;
	}

	/* walk on the columns */
	for (j=0; j < proj_map->nattrs; j++)
	{
		const kern_projection_desc *desc = &proj_map->desc[proj_map->nexprs + j];
		kern_colmeta   *cmeta = kcxt->kvars_cmeta[desc->slot_id];
		void		   *vaddr = kcxt->kvars_addr[desc->slot_id];
		int				vlen  = kcxt->kvars_len[desc->slot_id];
		char		   *dest  = NULL;

		if (!vaddr)
		{
			assert(t_hasnull);
			continue;
		}
		if (htup && t_hasnull)
			htup->t_bits[j>>3] |= (1<<(j & 7));

		if (cmeta)
		{
			/* adjust alignment */
			t_next = TYPEALIGN(cmeta->attalign, t_hoff);
			if (htup)
			{
				if (t_next > t_hoff)
					memset((char *)htup + t_hoff, 0, t_next - t_hoff);
				dest = (char *)htup + t_next;
			}
			/* move datum */
			if (desc->slot_ops &&
				desc->slot_ops->xpu_datum_move)
			{
				sz = desc->slot_ops->xpu_datum_move(kcxt, dest, cmeta, vaddr, vlen);
			}
			else if (cmeta->attlen > 0)
			{
				sz = cmeta->attlen;
				if (dest)
					memcpy(dest, vaddr, sz);
			}
			else if (cmeta->attlen == -1)
			{
				sz = VARSIZE_ANY(vaddr);
				if (dest)
					memcpy(dest, vaddr, sz);
			}
			else
			{
				STROM_ELOG(kcxt, "unexpected type length");
				return -1;
			}
			t_hoff = t_next + sz;
		}
		else
		{
			xpu_datum_t *datum = (xpu_datum_t *)vaddr;
			int8_t		valign = datum->ops->xpu_type_align;

			/* adjust alignment */
			t_next = TYPEALIGN(valign, t_hoff);
			if (htup)
			{
				if (t_next > t_hoff)
					memset((char *)htup + t_hoff, 0, t_next - t_hoff);
				dest = (char *)htup + t_next;
			}
			sz = datum->ops->xpu_datum_store(kcxt, dest, datum);
			if (sz < 0)
				return -1;
			t_hoff = t_next + sz;
		}
	}

	if (htup)
	{
		htup->t_infomask = t_infomask;
		SET_VARSIZE(&htup->t_choice.t_datum, t_hoff);
	}
	return t_hoff;	
}

STATIC_FUNCTION(bool)
pgfn_Projection(XPU_PGFUNCTION_ARGS)
{
	const kern_projection_map *proj_map = __KEXP_GET_PROJECTION_MAP(kexp);
	const kern_expression *karg;
	xpu_int4_t *result;
	int			i, sz;

	/* exec sub-expressions */
	for (i=0, karg = KEXP_FIRST_ARG(-1, Invalid);
		 i < kexp->nargs;
		 i++, karg = KEXP_NEXT_ARG(karg, Invalid))
	{
		const kern_projection_desc *desc = &proj_map->desc[i];
		xpu_datum_t *datum;

		assert(desc->slot_id < kcxt->kvars_nslots &&
			   desc->slot_type == karg->exptype_ops->xpu_type_code);

		datum = (xpu_datum_t *)kcxt_alloc(kcxt, karg->exptype_ops->xpu_type_sizeof);
		if (!datum)
		{
			STROM_ELOG(kcxt, "out of kcxt memory");
			return false;
		}
		if (!EXEC_KERN_EXPRESSION(kcxt, karg, datum))
			return false;
		kcxt->kvars_cmeta[desc->slot_id] = NULL;
		kcxt->kvars_addr[desc->slot_id]  = (!datum->isnull ? datum : NULL);
		kcxt->kvars_len[desc->slot_id]   = -1;
	}
	assert(i == proj_map->nexprs);

	/* then, estimate the length */
	sz = kern_form_heaptuple(kcxt, kexp, NULL, NULL);
	if (sz < 0)
		return false;
	/* setup the result */
	result = (xpu_int4_t *)__result;
	memset(result, 0, sizeof(xpu_int4_t));
	result->ops = &xpu_int4_ops;
	result->isnull = false;
	result->value  = offsetof(kern_tupitem, htup) + sz;

	return true;
}

/* ----------------------------------------------------------------
 *
 * LoadVars / Projection
 *
 * ----------------------------------------------------------------
 */
STATIC_FUNCTION(int)
kern_extract_heap_tuple(kern_context *kcxt,
						kern_data_store *kds,
						kern_tupitem *tupitem,
						int curr_depth,
						const kern_preload_vars_item *kvars_items,
						int kvars_nloads)
{
	const kern_preload_vars_item *kvars = kvars_items;
	HeapTupleHeaderData *htup = &tupitem->htup;
	uint32_t	offset;
	int			kvars_nloads_saved = kvars_nloads;
	int			resno = 1;
	int			slot_id;
	int			ncols = (htup->t_infomask2 & HEAP_NATTS_MASK);
	bool		heap_hasnull = ((htup->t_infomask & HEAP_HASNULL) != 0);

	/* shortcut if no columns in this depth */
	if (kvars->var_depth != curr_depth)
		return 0;

	if (ncols > kds->ncols)
		ncols = kds->ncols;
	/* try attcacheoff shortcut, if available. */
	if (!heap_hasnull)
	{
		while (kvars_nloads > 0 &&
			   kvars->var_depth == curr_depth &&
			   kvars->var_resno <= ncols)
		{
			kern_colmeta   *cmeta = &kds->colmeta[kvars->var_resno - 1];

			assert(resno <= kvars->var_resno);
			if (cmeta->attcacheoff < 0)
				break;
			slot_id = kvars->var_slot_id;
			resno   = kvars->var_resno;
			offset  = htup->t_hoff + cmeta->attcacheoff;
			assert(slot_id < kcxt->kvars_nslots);
			kcxt->kvars_cmeta[slot_id] = cmeta;
			kcxt->kvars_addr[slot_id]  = (char *)htup + offset;
			kcxt->kvars_len[slot_id]   = -1;
			kvars++;
			kvars_nloads--;
		}
	}

	/* move to the slow heap-tuple extract */
	offset = htup->t_hoff;
	while (kvars_nloads > 0 &&
		   kvars->var_depth == curr_depth &&
		   kvars->var_resno >= resno &&
		   kvars->var_resno <= ncols)
	{
		while (resno <= ncols)
		{
			kern_colmeta   *cmeta = &kds->colmeta[resno-1];
			char		   *addr;
			int				len;

			if (heap_hasnull && att_isnull(resno-1, htup->t_bits))
			{
				addr = NULL;
				len  = -1;
			}
			else
			{
				if (cmeta->attlen > 0)
					offset = TYPEALIGN(cmeta->attalign, offset);
				else if (!VARATT_NOT_PAD_BYTE((char *)htup + offset))
					offset = TYPEALIGN(cmeta->attalign, offset);

				addr = ((char *)htup + offset);
				if (cmeta->attlen > 0)
				{
					offset += cmeta->attlen;
					len = cmeta->attlen;
				}
				else
				{
					offset += VARSIZE_ANY(addr);
					if (VARATT_IS_COMPRESSED(addr) || VARATT_IS_EXTERNAL(addr))
						len = -1;
					else
					{
						addr = VARDATA_ANY(addr);
						len  = VARSIZE_ANY(addr);
					}
				}
			}

			if (kvars->var_resno == resno++)
			{
				slot_id = kvars->var_slot_id;
				assert(slot_id < kcxt->kvars_nslots);

				kcxt->kvars_cmeta[slot_id] = cmeta;
				kcxt->kvars_addr[slot_id]  = addr;
				kcxt->kvars_len[slot_id]   = len;
				kvars++;
				kvars_nloads--;
				break;
			}
		}
	}
	/* other fields, which refers out of ranges, are NULL */
	while (kvars_nloads > 0 &&
		   kvars->var_depth == curr_depth)
	{
		kern_colmeta *cmeta = &kds->colmeta[kvars->var_resno-1];
		int		slot_id = kvars->var_slot_id;

		assert(slot_id < kcxt->kvars_nslots);
		kcxt->kvars_cmeta[slot_id] = cmeta;
		kcxt->kvars_addr[slot_id]  = NULL;
		kcxt->kvars_len[slot_id]   = -1;
		kvars++;
		kvars_nloads--;
	}
	return (kvars_nloads_saved - kvars_nloads);
}

/*
 * Routines to extract Arrow data store
 */
INLINE_FUNCTION(bool)
arrow_bitmap_check(kern_data_store *kds,
				   uint32_t kds_index,
				   uint32_t bitmap_offset,
				   uint32_t bitmap_length)
{
	uint8_t	   *bitmap;
	uint8_t		mask = (1<<(kds_index & 7));
	uint32_t	idx = (kds_index >> 3);

	if (bitmap_offset == 0 ||	/* no bitmap */
		bitmap_length == 0 ||	/* no bitmap */
		idx >= __kds_unpack(bitmap_length))		/* out of range */
		return false;
	bitmap = (uint8_t *)kds + __kds_unpack(bitmap_offset);

	return (bitmap[idx] & mask) != 0;
}

STATIC_FUNCTION(bool)
arrow_fetch_secondary_index(kern_context *kcxt,
							kern_data_store *kds,
							uint32_t kds_index,
							uint32_t values_offset,
							uint32_t values_length,
							bool is_large_offset,
							uint64_t *p_start,
							uint64_t *p_end)
{
	char	   *base = (char *)kds + __kds_unpack(values_offset);
	char	   *extra = (char *)kds + __kds_unpack(values_length);
	uint64_t	start, end;

	if (!values_offset || !values_length)
	{
		STROM_ELOG(kcxt, "Arrow variable index/buffer is missing");
		return false;
	}

	if (is_large_offset)
	{
		if (sizeof(uint64_t) * (kds_index+2) > __kds_unpack(values_length))
		{
			STROM_ELOG(kcxt, "Arrow variable index[64bit] out of range");
			return false;
		}
		start = ((uint64_t *)base)[kds_index];
		end = ((uint64_t *)base)[kds_index+1];
	}
	else
	{
		if (sizeof(uint32_t) * (kds_index+2) > __kds_unpack(values_length))
		{
			STROM_ELOG(kcxt, "Arrow variable index[32bit] out of range");
			return false;
		}
		start = ((uint32_t *)base)[kds_index];
		end = ((uint32_t *)base)[kds_index+1];
	}
	*p_start = start;
	*p_end = end;
	return true;
}

INLINE_FUNCTION(bool)
__arrow_fetch_bool_datum(kern_context *kcxt,
						 kern_data_store *kds,
						 kern_colmeta *cmeta,
						 uint32_t kds_index,
						 void **p_addr)
{
	xpu_bool_t	   *datum;

	assert(cmeta->atttypid == PG_BOOLOID &&
		   cmeta->extra_offset == 0 &&
		   cmeta->extra_length == 0);
	if (cmeta->nullmap_offset &&
		!arrow_bitmap_check(kds, kds_index,
							cmeta->nullmap_offset,
							cmeta->nullmap_length))
	{
		*p_addr = NULL;
		return true;
	}

	datum = (xpu_bool_t *)kcxt_alloc(kcxt, sizeof(xpu_bool_t));
	if (!datum)
	{
		STROM_ELOG(kcxt, "out of memory");
		return false;
	}
	datum->ops = &xpu_bool_ops;
	datum->isnull = false;
	datum->value = arrow_bitmap_check(kds, kds_index,
									  cmeta->values_offset,
									  cmeta->values_length);
	*p_addr = datum;
	return true;
}

INLINE_FUNCTION(bool)
__arrow_fetch_inline_datum(kern_context *kcxt,
						   kern_data_store *kds,
						   kern_colmeta *cmeta,
						   uint32_t kds_index,
						   void **p_addr,
						   int *p_len)
{
	int			unitsz = cmeta->attopts.common.unitsz;

	if (unitsz <= 0)
	{
		STROM_ELOG(kcxt, "Apache Arrow incorrect unit size");
		return false;
	}
	if (cmeta->nullmap_offset)
	{
		if (!arrow_bitmap_check(kds, kds_index,
								cmeta->nullmap_offset,
								cmeta->nullmap_length))
		{
			*p_addr = NULL;
			*p_len  = unitsz;
			return true;
		}
	}
	if (cmeta->values_offset &&
		unitsz * (kds_index+1) <= __kds_unpack(cmeta->values_length))
	{
		char   *base = (char *)kds + __kds_unpack(cmeta->values_offset);

		*p_addr = base + unitsz * kds_index;
		*p_len  = unitsz;
		return true;
	}
	STROM_ELOG(kcxt, "Arrow inline value out of range");
	return false;
}

STATIC_FUNCTION(bool)
__arrow_fetch_variable_datum(kern_context *kcxt,
							 kern_data_store *kds,
							 kern_colmeta *cmeta,
							 uint32_t kds_index,
							 bool is_large_offset,
							 void **p_addr,
							 int *p_len)
{
	char	   *extra;
	uint64_t	start, end;

	if (cmeta->nullmap_offset)
	{
		if (!arrow_bitmap_check(kds, kds_index,
								cmeta->nullmap_offset,
								cmeta->nullmap_length))
		{
			*p_addr = NULL;
			*p_len  = -1;
			return true;
		}
	}
	if (!arrow_fetch_secondary_index(kcxt, kds, kds_index,
									 cmeta->values_offset,
									 cmeta->values_length,
									 is_large_offset,
									 &start, &end))
		return false;
	/* sanity checks */
	if (start > end || end - start >= 0x40000000UL ||
		end > __kds_unpack(cmeta->extra_length))
	{
		STROM_ELOG(kcxt, "Arrow variable data corruption");
		return false;
	}
	extra = (char *)kds + __kds_unpack(cmeta->extra_offset);
	*p_addr = extra + start;
	*p_len  = end - start;
	return true;
}

STATIC_FUNCTION(bool)
__arrow_fetch_array_datum(kern_context *kcxt,
						  kern_data_store *kds,
						  kern_colmeta *cmeta,
						  uint32_t kds_index,
						  bool is_large_offset,
						  void **p_addr)
{
	xpu_array_t	   *datum = NULL;
	uint64_t		start, end;

	if (cmeta->nullmap_offset &&
		!arrow_bitmap_check(kds, kds_index,
							cmeta->nullmap_offset,
							cmeta->nullmap_length))
	{
		*p_addr = NULL;
		return true;
	}

	assert(cmeta->idx_subattrs < kds->nr_colmeta &&
		   cmeta->num_subattrs == 1);
	if (!arrow_fetch_secondary_index(kcxt, kds, kds_index,
									 cmeta->values_offset,
									 cmeta->values_length,
									 is_large_offset,
									 &start, &end))
		return false;
	/* sanity checks */
	if (start > end)
	{
		STROM_ELOG(kcxt, "Arrow secondary index corruption");
		return false;
	}

	datum = (xpu_array_t *)kcxt_alloc(kcxt, sizeof(xpu_array_t));
	if (!datum)
	{
		STROM_ELOG(kcxt, "out of memory");
		return false;
	}
	memset(datum, 0, sizeof(xpu_array_t));

	//datum->ops = &xpu_array_ops;
	datum->value  = (char *)kds;
	datum->start  = start;
	datum->length = end - start;
	datum->smeta  = &kds->colmeta[cmeta->idx_subattrs];

	*p_addr = datum;
	return true;
}

INLINE_FUNCTION(bool)
__arrow_fetch_composite_datum(kern_context *kcxt,
							  kern_data_store *kds,
							  kern_colmeta *cmeta,
							  uint32_t kds_index,
							  void **p_addr)
{
	xpu_composite_t	*datum;

	if (cmeta->nullmap_offset &&
		!arrow_bitmap_check(kds, kds_index,
							cmeta->nullmap_offset,
							cmeta->nullmap_length))
	{
		*p_addr = NULL;
		return true;
	}

	datum = (xpu_composite_t *)kcxt_alloc(kcxt, sizeof(xpu_composite_t));
	if (!datum)
	{
		STROM_ELOG(kcxt, "out of memory");
		return false;
	}
	memset(datum, 0, sizeof(xpu_composite_t));
	//datum->ops = &xpu_composite_ops;
	datum->nfields = cmeta->num_subattrs;
	datum->rowidx  = kds_index;
	datum->value   = (char *)kds;
	datum->smeta   = &kds->colmeta[cmeta->idx_subattrs];

	*p_addr = datum;
	return true;
}

STATIC_FUNCTION(int)
kern_extract_arrow_tuple(kern_context *kcxt,
						 kern_data_store *kds,
						 uint32_t kds_index,
						 const kern_preload_vars_item *kvars_items,
						 int kvars_nloads)
{
	const kern_preload_vars_item *kvars = kvars_items;
	int		kvars_nloads_saved = kvars_nloads;

	assert(kds->format == KDS_FORMAT_ARROW);
	while (kvars_nloads > 0 &&
		   kvars->var_depth == 0 &&
		   kvars->var_resno <= kds->ncols)
	{
		kern_colmeta *cmeta = &kds->colmeta[kvars->var_resno-1];
		int			slot_id = kvars->var_slot_id;
		void	   *addr = NULL;
		int			len = -1;

		assert(slot_id < kcxt->kvars_nslots);
		switch (cmeta->attopts.common.tag)
		{
			case ArrowNodeTag__Bool:
				/* xpu_bool_t */
				if (!__arrow_fetch_bool_datum(kcxt,
											  kds,
											  cmeta,
											  kds_index,
											  &addr))
					return -1;
				if (addr)
					cmeta = NULL;	/* mark addr points xpu_datum_t */
				break;

			case ArrowNodeTag__Int:
			case ArrowNodeTag__FloatingPoint:
			case ArrowNodeTag__Decimal:
			case ArrowNodeTag__Date:
			case ArrowNodeTag__Time:
			case ArrowNodeTag__Timestamp:
			case ArrowNodeTag__Interval:
			case ArrowNodeTag__FixedSizeBinary:
				if (!__arrow_fetch_inline_datum(kcxt,
												kds,
												cmeta,
												kds_index,
												&addr,
												&len))
					return -1;
				break;

			case ArrowNodeTag__Utf8:
			case ArrowNodeTag__Binary:
				if (!__arrow_fetch_variable_datum(kcxt,
												  kds,
												  cmeta,
												  kds_index,
												  false,
												  &addr,
												  &len))
					return -1;
				break;
			case ArrowNodeTag__LargeUtf8:
			case ArrowNodeTag__LargeBinary:
				if (!__arrow_fetch_variable_datum(kcxt,
												  kds,
												  cmeta,
												  kds_index,
												  true,
												  &addr,
												  &len))
					return -1;
				break;

			case ArrowNodeTag__List:
				/* xpu_array_t */
				if (!__arrow_fetch_array_datum(kcxt,
											   kds,
											   cmeta,
											   kds_index,
											   false,
											   &addr))
					return -1;
				if (addr)
					cmeta = NULL;	/* mark addr points xpu_array_t */
				break;

			case ArrowNodeTag__LargeList:
				/* xpu_array_t */
				if (!__arrow_fetch_array_datum(kcxt,
											   kds,
											   cmeta,
											   kds_index,
											   true,
											   &addr))
					return -1;
				if (addr)
					cmeta = NULL;	/* mark addr points xpu_array_t */
				break;

			case ArrowNodeTag__Struct:	/* composite */
				/* xpu_composite_t */
				if (!__arrow_fetch_composite_datum(kcxt,
												   kds,
												   cmeta,
												   kds_index,
												   &addr))
					return -1;
				if (addr)
					cmeta = NULL;	/* mark addr points xpu_composite_t */
				break;

			default:
				STROM_ELOG(kcxt, "Unsupported Apache Arrow type");
				return -1;
		}
		kcxt->kvars_cmeta[slot_id] = cmeta;
		kcxt->kvars_addr[slot_id]  = addr;
		kcxt->kvars_len[slot_id]   = len;
		kvars++;
		kvars_nloads--;
	}
	/* other fields, which refers out of range, are NULL */
	while (kvars_nloads > 0 && kvars->var_depth == 0)
	{
		kern_colmeta *cmeta = &kds->colmeta[kvars->var_resno-1];
		int		slot_id = kvars->var_slot_id;

		assert(slot_id < kcxt->kvars_nslots);
		kcxt->kvars_cmeta[slot_id] = cmeta;
		kcxt->kvars_addr[slot_id]  = NULL;
		kcxt->kvars_len[slot_id]   = -1;
		kvars++;
		kvars_nloads--;
	}
	return (kvars_nloads_saved - kvars_nloads);
}

STATIC_FUNCTION(bool)
pgfn_LoadVars(XPU_PGFUNCTION_ARGS)
{
	STROM_ELOG(kcxt, "Bug? LoadVars shall not be called as a part of expression");
	return false;
}

PUBLIC_FUNCTION(bool)
ExecLoadVarsOuterRow(XPU_PGFUNCTION_ARGS,
					 kern_data_store *kds_outer,
					 kern_tupitem *tupitem_outer,
					 int num_inners,
					 kern_data_store **kds_inners,
					 kern_tupitem **tupitem_inners)
{
	const kern_preload_vars *preload;
	const kern_expression *karg;
	int			index;
	int			depth;

	assert(kexp->opcode == FuncOpCode__LoadVars &&
		   kexp->nargs == 1);
	karg = (const kern_expression *)kexp->u.data;
	assert(kexp->exptype == karg->exptype);
	preload = (const kern_preload_vars *)((const char *)karg +
										  MAXALIGN(VARSIZE(karg)));
	/*
	 * Walking on the outer/inner tuples
	 */
	for (depth=0, index=0;
		 depth <= num_inners && index < preload->nloads;
		 depth++)
	{
		kern_data_store *kds;
		kern_tupitem *tupitem;

		if (depth == 0)
		{
			kds     = kds_outer;
			tupitem = tupitem_outer;
		}
		else
		{
			kds     = kds_inners[depth - 1];
			tupitem = tupitem_inners[depth - 1];
		}
		index += kern_extract_heap_tuple(kcxt,
										 kds,
										 tupitem,
										 depth,
										 preload->kvars + index,
										 preload->nloads - index);
	}
	return EXEC_KERN_EXPRESSION(kcxt, karg, __result);
}

PUBLIC_FUNCTION(bool)
ExecLoadVarsOuterArrow(XPU_PGFUNCTION_ARGS,
                       kern_data_store *kds_outer,
                       uint32_t kds_index,
                       int num_inners,
                       kern_data_store **kds_inners,
                       kern_tupitem **tupitem_inners)
{
	const kern_expression *karg = (const kern_expression *)kexp->u.data;
	const kern_preload_vars *preload;
	int			index;
	int			depth;
	int			count;

	assert(kexp->opcode == FuncOpCode__LoadVars &&
		   kexp->nargs == 1 &&
		   kexp->exptype == karg->exptype);
	preload = (const kern_preload_vars *)((char *)karg + MAXALIGN(VARSIZE(karg)));

	/*
	 * Walking on the outer/inner tuples
	 */
	for (depth=0, index=0;
		 depth <= num_inners && index < preload->nloads;
		 depth++)
	{
		if (depth == 0)
		{
			count = kern_extract_arrow_tuple(kcxt,
											 kds_outer,
											 kds_index,
											 preload->kvars + index,
											 preload->nloads - index);
		}
		else
		{
			count = kern_extract_heap_tuple(kcxt,
											kds_inners[depth - 1],
											tupitem_inners[depth - 1],
											depth,
											preload->kvars + index,
											preload->nloads - index);
		}
		if (count < 0)
			return false;
		index += count;
	}
	return EXEC_KERN_EXPRESSION(kcxt, karg, __result);
}

/*
 * Catalog of built-in device types
 */
/*
 * Built-in SQL type / function catalog
 */
#define TYPE_OPCODE(NAME,a,b)							\
	{ TypeOpCode__##NAME, &xpu_##NAME##_ops },
PUBLIC_DATA xpu_type_catalog_entry builtin_xpu_types_catalog[] = {
#include "xpu_opcodes.h"
	{ TypeOpCode__Invalid, NULL }
};

/*
 * Catalog of built-in device functions
 */
#define FUNC_OPCODE(a,b,c,NAME,d,e)				\
	{FuncOpCode__##NAME, pgfn_##NAME},
PUBLIC_DATA xpu_function_catalog_entry builtin_xpu_functions_catalog[] = {
	{FuncOpCode__ConstExpr, 				pgfn_ConstExpr },
	{FuncOpCode__ParamExpr, 				pgfn_ParamExpr },
    {FuncOpCode__VarExpr,					pgfn_VarExpr },
    {FuncOpCode__BoolExpr_And,				pgfn_BoolExprAnd },
    {FuncOpCode__BoolExpr_Or,				pgfn_BoolExprOr },
    {FuncOpCode__BoolExpr_Not,				pgfn_BoolExprNot },
    {FuncOpCode__NullTestExpr_IsNull,		pgfn_NullTestExpr },
    {FuncOpCode__NullTestExpr_IsNotNull,	pgfn_NullTestExpr },
    {FuncOpCode__BoolTestExpr_IsTrue,		pgfn_BoolTestExpr},
    {FuncOpCode__BoolTestExpr_IsNotTrue,	pgfn_BoolTestExpr},
    {FuncOpCode__BoolTestExpr_IsFalse,		pgfn_BoolTestExpr},
    {FuncOpCode__BoolTestExpr_IsNotFalse,	pgfn_BoolTestExpr},
    {FuncOpCode__BoolTestExpr_IsUnknown,	pgfn_BoolTestExpr},
    {FuncOpCode__BoolTestExpr_IsNotUnknown,	pgfn_BoolTestExpr},
#include "xpu_opcodes.h"
	{FuncOpCode__Projection,                pgfn_Projection},
	{FuncOpCode__LoadVars,                  pgfn_LoadVars},
	{FuncOpCode__Invalid, NULL},
};

/*
 * Device version of hash_any() in PG host code
 */
#define rot(x,k)		(((x)<<(k)) | ((x)>>(32-(k))))
#define mix(a,b,c)								\
	{											\
		a -= c;  a ^= rot(c, 4);  c += b;		\
		b -= a;  b ^= rot(a, 6);  a += c;		\
		c -= b;  c ^= rot(b, 8);  b += a;		\
		a -= c;  a ^= rot(c,16);  c += b;		\
		b -= a;  b ^= rot(a,19);  a += c;		\
		c -= b;  c ^= rot(b, 4);  b += a;		\
	}

#define final(a,b,c)							\
	{											\
		c ^= b; c -= rot(b,14);					\
		a ^= c; a -= rot(c,11);					\
		b ^= a; b -= rot(a,25);					\
		c ^= b; c -= rot(b,16);					\
		a ^= c; a -= rot(c, 4);					\
		b ^= a; b -= rot(a,14);					\
		c ^= b; c -= rot(b,24);					\
	}

PUBLIC_FUNCTION(uint32_t)
pg_hash_any(const void *ptr, int sz)
{
	const uint8_t  *k = (const uint8_t *)ptr;
	uint32_t		a, b, c;
	uint32_t		len = sz;

	/* Set up the internal state */
	a = b = c = 0x9e3779b9 + len + 3923095;

	/* If the source pointer is word-aligned, we use word-wide fetches */
	if (((uint64_t) k & (sizeof(uint32_t) - 1)) == 0)
	{
		/* Code path for aligned source data */
		const uint32_t	*ka = (const uint32_t *) k;

		/* handle most of the key */
		while (len >= 12)
		{
			a += ka[0];
			b += ka[1];
			c += ka[2];
			mix(a, b, c);
			ka += 3;
			len -= 12;
		}

		/* handle the last 11 bytes */
		k = (const unsigned char *) ka;
		switch (len)
		{
			case 11:
				c += ((uint32_t) k[10] << 24);
				/* fall through */
			case 10:
				c += ((uint32_t) k[9] << 16);
				/* fall through */
			case 9:
				c += ((uint32_t) k[8] << 8);
				/* the lowest byte of c is reserved for the length */
				/* fall through */
			case 8:
				b += ka[1];
				a += ka[0];
				break;
			case 7:
				b += ((uint32_t) k[6] << 16);
				/* fall through */
			case 6:
				b += ((uint32_t) k[5] << 8);
				/* fall through */
			case 5:
				b += k[4];
				/* fall through */
			case 4:
				a += ka[0];
				break;
			case 3:
				a += ((uint32_t) k[2] << 16);
				/* fall through */
			case 2:
				a += ((uint32_t) k[1] << 8);
				/* fall through */
			case 1:
				a += k[0];
				/* case 0: nothing left to add */
		}
	}
	else
	{
		/* Code path for non-aligned source data */

		/* handle most of the key */
		while (len >= 12)
		{
			a += k[0] + (((uint32_t) k[1] << 8) +
						 ((uint32_t) k[2] << 16) +
						 ((uint32_t) k[3] << 24));
			b += k[4] + (((uint32_t) k[5] << 8) +
						 ((uint32_t) k[6] << 16) +
						 ((uint32_t) k[7] << 24));
			c += k[8] + (((uint32_t) k[9] << 8) +
						 ((uint32_t) k[10] << 16) +
						 ((uint32_t) k[11] << 24));
			mix(a, b, c);
			k += 12;
			len -= 12;
		}

		/* handle the last 11 bytes */
		switch (len)            /* all the case statements fall through */
		{
			case 11:
				c += ((uint32_t) k[10] << 24);
			case 10:
				c += ((uint32_t) k[9] << 16);
			case 9:
				c += ((uint32_t) k[8] << 8);
				/* the lowest byte of c is reserved for the length */
			case 8:
				b += ((uint32_t) k[7] << 24);
			case 7:
				b += ((uint32_t) k[6] << 16);
			case 6:
				b += ((uint32_t) k[5] << 8);
			case 5:
				b += k[4];
			case 4:
				a += ((uint32_t) k[3] << 24);
			case 3:
				a += ((uint32_t) k[2] << 16);
			case 2:
				a += ((uint32_t) k[1] << 8);
			case 1:
				a += k[0];
				/* case 0: nothing left to add */
		}
	}
	final(a, b, c);

	return c;
}
#undef rot
#undef mix
#undef final
