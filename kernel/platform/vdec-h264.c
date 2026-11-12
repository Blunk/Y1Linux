// SPDX-License-Identifier: GPL-2.0-only
/* MT6582 video decoder: one progressive H.264 picture.
 *
 * The register sequence is the one the stock firmware's libvcodecdrv.so runs (function names in the
 * comments are that library's), read out in docs/knowledge/video-decoder.md. The hardware reads the
 * slice data and the following slices of the picture itself; the first slice header is walked through
 * its bit reader so that reference list modification and the weight table, which the hardware parses,
 * start at the right bit. */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/sort.h>
#include "vdec.h"

#define H264_ERROR_MASK 0xbfdfb2f7U
#define H264_VLD_MODE 0xae00e007U
#define POC_MASK 0x3ffffU

enum { SLICE_P, SLICE_B, SLICE_I, SLICE_SP, SLICE_SI };

struct h264_slice {
	u32 first_mb, type, kind; /* type as coded (0..9), kind = type % 5 */
	u32 l0, l1;               /* active reference counts minus one */
	u32 direct_spatial, cabac_init, deblock_idc;
	s32 qp_delta, alpha, beta;
};

/* The hardware's bit reader: reading AVC_VLD + 4n returns the next 32 bits and consumes n of them. */
static u32 h264_bits(struct y2_vdec *v, unsigned n)
{
	u32 window = vdec_read(v, VDEC_AVLD(0));
	vdec_read(v, VDEC_AVLD(4 * n));
	return window >> (32 - n);
}
static u32 h264_ue(struct y2_vdec *v) { return vdec_read(v, VDEC_AVLD(0x23c)); }
static s32 h264_se(struct y2_vdec *v) { return (s32)vdec_read(v, VDEC_AVLD(0x240)); }

static int h264_wait(struct y2_vdec *v, unsigned reg, u32 mask, u32 want, unsigned us)
{
	u32 value;
	return readl_poll_timeout_atomic(v->regs + reg, value, (value & mask) == want, 1, us);
}

/* H264_HAL_ResetHW and H264_HAL_InitVDecHW. */
static void h264_reset(struct y2_vdec *v)
{
	if (vdec_read(v, VDEC_VLD(0xf4)) & BIT(15))
		h264_wait(v, VDEC_VLD(0xf4), BIT(0), BIT(0), 50000);
	vdec_write(v, VDEC_VLD(0x108), 0x101);
	writel(1, v->sys);
	vdec_write(v, VDEC_MISC(0xf4), vdec_read(v, VDEC_MISC(0xf4)) & ~1U);
	vdec_write(v, VDEC_MISC(0x84), 2);
	vdec_write(v, VDEC_MISC(0x0), (vdec_read(v, VDEC_MISC(0x0)) & 0xff400000) | 0x00801ff0);
	vdec_write(v, VDEC_MISC(0xc8), 0xf7);
	vdec_write(v, VDEC_MISC(0xcc), 0x13a20100);
	vdec_write(v, VDEC_VLD(0x108), 0);
	writel(1, v->sys + 0x18);
	vdec_write(v, VDEC_MISC(0xec), 1);
	vdec_write(v, VDEC_AVLD(0x280), H264_ERROR_MASK);
}

/* H264_HAL_InitH264BarrelShift, …BarrelShifterAlign and H264_HAL_GetStartCode_8530: point the bit
 * reader at the bitstream buffer and step over the first start code and NAL header. */
static int h264_feed(struct y2_vdec *v, u32 size)
{
	u32 start = v->bits.dma, end = start + size + 0x100;
	vdec_write(v, VDEC_VLD(0xc0), 0x40000);
	vdec_write(v, VDEC_AVLD(0x84), H264_VLD_MODE);
	vdec_set(v, VDEC_TOP(0x54), 1);
	vdec_write(v, VDEC_MC(0x24), 6);
	vdec_write(v, VDEC_VLD(0xb4), start >> 6);
	vdec_write(v, VDEC_VLD(0xb8), end >> 6);
	vdec_write(v, VDEC_TOP(0x28), v->work.dma);
	vdec_write(v, VDEC_AVLD(0x28c), 1);
	vdec_write(v, VDEC_AVLD(0x28c), 0);
	if (vdec_read(v, VDEC_VLD(0xf4)) & BIT(15))
		h264_wait(v, VDEC_VLD(0xf4), BIT(0), BIT(0), 50000);
	vdec_set(v, VDEC_VLD(0x110), 1);
	vdec_write(v, VDEC_VLD(0xb0), start);
	vdec_write(v, VDEC_VLD(0xb0), start);
	vdec_write(v, VDEC_VLD(0x110), end);
	vdec_write(v, VDEC_AVLD(0x28c), 1);
	vdec_write(v, VDEC_VLD(0x108), 0x100);
	vdec_write(v, VDEC_VLD(0x108), 0);
	vdec_write(v, VDEC_VLD(0xcc), 0);
	vdec_write(v, VDEC_VLD(0x8c), 0x100000);
	if (h264_wait(v, VDEC_VLD(0xe8), BIT(0), BIT(0), 20000))
		return -EIO;
	vdec_write(v, VDEC_VLD(0x8c), 0x800000);
	vdec_write(v, VDEC_AVLD(0x28c), 0);
	v->trace[0] = vdec_read(v, VDEC_AVLD(0));
	/* H264_HAL_GetStartCode_8530: search only when the reader is not on a start code already
	 * (a search from a start code runs on to the next one). */
	v->trace[1] = vdec_read(v, VDEC_AVLD(0x84));
	if (v->trace[0] >> 8 != 1 || (s32)v->trace[1] < 0) {
		vdec_write(v, VDEC_AVLD(0x2d8), 1);
		v->trace[2] = h264_wait(v, VDEC_AVLD(0x2d8), BIT(0), 0, 100000);
		if (v->trace[2] && !(vdec_read(v, VDEC_AVLD(0x2c8)) & BIT(11))) return -EILSEQ;
	}
	v->trace[3] = vdec_read(v, VDEC_AVLD(0));
	h264_bits(v, 32);
	v->trace[4] = vdec_read(v, VDEC_AVLD(0));
	return 0;
}

/* A reference as the lists order it. */
struct h264_ref {
	const struct y2_vdec_h264_ref *ref;
	s32 number; /* FrameNumWrap, or LongTermFrameIdx */
	s32 poc;
};
static bool h264_long(const struct h264_ref *r) { return r->ref->flags & Y2_VDEC_H264_REF_LONG; }

static int h264_by_number_desc(const void *a, const void *b)
{
	return ((const struct h264_ref *)b)->number - ((const struct h264_ref *)a)->number;
}
static int h264_by_number_asc(const void *a, const void *b)
{
	return ((const struct h264_ref *)a)->number - ((const struct h264_ref *)b)->number;
}
static int h264_by_poc_desc(const void *a, const void *b)
{
	return ((const struct h264_ref *)b)->poc - ((const struct h264_ref *)a)->poc;
}
static int h264_by_poc_asc(const void *a, const void *b)
{
	return ((const struct h264_ref *)a)->poc - ((const struct h264_ref *)b)->poc;
}

struct h264_lists {
	struct h264_ref p[16], b0[16], b1[16];
	unsigned count;
};

/* The initial lists of the standard (8.2.4.2.1, 8.2.4.2.3) for a frame: P by PicNum descending, B by
 * picture order around the current picture, long-term references last by LongTermPicNum ascending. */
static void h264_build_lists(const struct y2_vdec_h264 *p, struct h264_lists *l)
{
	struct h264_ref shorts[16], longs[16];
	s32 max_frame_num = 1 << p->log2_max_frame_num, now = min(p->top_poc, p->bottom_poc);
	unsigned i, n_short = 0, n_long = 0, before = 0;
	for (i = 0; i < p->refs; i++) {
		const struct y2_vdec_h264_ref *r = &p->ref[i];
		struct h264_ref e = { .ref = r, .number = r->number, .poc = min(r->top_poc, r->bottom_poc) };
		if (r->flags & Y2_VDEC_H264_REF_LONG) {
			longs[n_long++] = e;
		} else {
			if (e.number > p->frame_num) e.number -= max_frame_num;
			shorts[n_short++] = e;
		}
	}
	sort(longs, n_long, sizeof(*longs), h264_by_number_asc, NULL);
	sort(shorts, n_short, sizeof(*shorts), h264_by_number_desc, NULL);
	memcpy(l->p, shorts, n_short * sizeof(*shorts));
	/* B: list 0 is the earlier pictures nearest first, then the later ones nearest first; list 1 the
	 * other way round. */
	sort(shorts, n_short, sizeof(*shorts), h264_by_poc_asc, NULL);
	while (before < n_short && shorts[before].poc < now) before++;
	memcpy(l->b1, shorts + before, (n_short - before) * sizeof(*shorts));
	memcpy(l->b0 + before, shorts + before, (n_short - before) * sizeof(*shorts));
	sort(shorts, before, sizeof(*shorts), h264_by_poc_desc, NULL);
	memcpy(l->b0, shorts, before * sizeof(*shorts));
	memcpy(l->b1 + n_short - before, shorts, before * sizeof(*shorts));
	memcpy(l->p + n_short, longs, n_long * sizeof(*longs));
	memcpy(l->b0 + n_short, longs, n_long * sizeof(*longs));
	memcpy(l->b1 + n_short, longs, n_long * sizeof(*longs));
	l->count = n_short + n_long;
	/* H264_HAL_B1ListSwap: a list 1 equal to list 0 has its first two entries exchanged. */
	if (l->count > 1 && !memcmp(l->b0, l->b1, l->count * sizeof(*l->b0)))
		swap(l->b1[0], l->b1[1]);
}

/* The words every list entry is made of. */
static u32 h264_ref_number(const struct h264_ref *r)
{
	return (h264_long(r) ? BIT(19) : 0) | (r->number & 0x7ffff);
}
static u32 h264_ref_field(const struct h264_ref *r, bool bottom, bool with_poc)
{
	const struct y2_vdec_h264_ref *ref = r->ref;
	u32 word = (u32)ref->frame << 23 | (h264_long(r) ? BIT(20) : 0);
	if (ref->top_poc <= ref->bottom_poc && ref->top_poc != S32_MAX) word |= BIT(21);
	if (bottom) word |= BIT(22);
	if (with_poc) word += (bottom ? ref->bottom_poc : ref->top_poc) & POC_MASK;
	return word;
}

/* H264_HAL_InitPRefList, …SetPRefPicListReg(_Extra). */
static void h264_write_p_list(struct y2_vdec *v, const struct y2_vdec_h264 *p,
			      const struct h264_lists *l)
{
	u32 y = v->frame[p->frame].dma, mv = v->mv[p->frame].dma >> 4;
	unsigned i;
	vdec_write(v, VDEC_MC(0x564), 0);
	vdec_write(v, VDEC_AVLD(0x228), 1);
	h264_wait(v, VDEC_AVLD(0x228), BIT(0), 0, 2000);
	vdec_write(v, VDEC_AVLD(0xa0), 1U << p->log2_max_frame_num);
	vdec_write(v, VDEC_AVLD(0xa4), p->frame_num);
	for (i = 0; i < 32; i++) vdec_write(v, VDEC_MC(0x3dc + 4 * i), v->last_y ?: y);
	for (i = 0; i < 32; i++) vdec_write(v, VDEC_AMV(0x180 + 4 * i), mv);
	for (i = 0; i < l->count; i++) {
		const struct h264_ref *r = &l->p[i];
		vdec_write(v, VDEC_MC(0x3dc + 4 * i), v->frame[r->ref->frame].dma);
		vdec_write(v, VDEC_AVLD(0xa8 + 4 * i), h264_ref_number(r));
		vdec_write(v, VDEC_AMV(8 * i), h264_ref_field(r, false, false));
		vdec_write(v, VDEC_AMV(8 * i + 4), h264_ref_field(r, true, false));
	}
}

/* H264_HAL_SetPOC, H264_HAL_InitBRefList, …SetBRefPicListReg(_Extra). */
static void h264_write_b_lists(struct y2_vdec *v, const struct y2_vdec_h264 *p,
			       const struct h264_lists *l)
{
	u32 y = v->last_y ?: v->frame[p->frame].dma;
	u32 colocated = p->flags & Y2_VDEC_H264_DIRECT_8X8 ? 2 : 4;
	unsigned i;
	vdec_write(v, VDEC_AMV(0x204), p->top_poc & POC_MASK);
	vdec_write(v, VDEC_AMV(0x208), p->bottom_poc & POC_MASK);
	vdec_write(v, VDEC_AMV(0x200), min(p->top_poc, p->bottom_poc) & POC_MASK);
	vdec_write(v, VDEC_MC(0x568), 0);
	vdec_write(v, VDEC_MC(0x56c), 0);
	for (i = 0; i < 32; i++) vdec_write(v, VDEC_MC(0x45c + 4 * i), y);
	for (i = 0; i < 32; i++) vdec_write(v, VDEC_MC(0x4dc + 4 * i), y);
	if (p->flags & Y2_VDEC_H264_DIRECT_8X8) vdec_set(v, VDEC_AMV(0x220), 0x20);
	for (i = 0; i < l->count; i++) {
		const struct h264_ref *r0 = &l->b0[i], *r1 = &l->b1[i];
		u32 mv = v->mv[r1->ref->frame].dma >> 4;
		vdec_write(v, VDEC_MC(0x45c + 4 * i), v->frame[r0->ref->frame].dma);
		vdec_write(v, VDEC_AVLD(0x128 + 4 * i), h264_ref_number(r0));
		vdec_write(v, VDEC_AMV(0x80 + 8 * i), h264_ref_field(r0, false, true));
		vdec_write(v, VDEC_AMV(0x84 + 8 * i), h264_ref_field(r0, true, true));
		vdec_write(v, VDEC_MC(0x4dc + 4 * i), v->frame[r1->ref->frame].dma);
		vdec_write(v, VDEC_AVLD(0x1a8 + 4 * i), h264_ref_number(r1));
		vdec_write(v, VDEC_AMV(0x100 + 8 * i), h264_ref_field(r1, false, true));
		vdec_write(v, VDEC_AMV(0x104 + 8 * i), h264_ref_field(r1, true, true));
		vdec_write(v, VDEC_AMV(0x180 + 8 * i), mv);
		vdec_write(v, VDEC_AMV(0x184 + 8 * i), mv + colocated);
	}
}

/* H264_HAL_WriteScalingList: `words` four-byte words at table address `at`. */
static void h264_write_scaling(struct y2_vdec *v, unsigned at, const u8 *list, unsigned words)
{
	unsigned k;
	for (k = 0; k < words; k++, list += 4) {
		vdec_write(v, VDEC_VLD(0x260), at + 4 * k + 0x200);
		vdec_write(v, VDEC_VLD(0x264), (u32)list[0] << 24 | list[1] << 16 | list[2] << 8 | list[3]);
		vdec_write(v, VDEC_VLD(0x260), at + 4 * k + 0x100);
	}
}

/* H264_HAL_SetSPSAVLD, …SetPPSAVLD, …SetSHDRAVLD1. */
static void h264_write_parameters(struct y2_vdec *v, const struct y2_vdec_h264 *p,
				  const struct h264_slice *s)
{
	u32 f = p->flags, scaling = 0, word;
	unsigned n;
	if (f & Y2_VDEC_H264_SCALING) {
		for (n = 0; n < 6; n++) h264_write_scaling(v, 16 * n, p->scaling4[n], 4);
		h264_write_scaling(v, 0x60, p->scaling8[0], 16);
		h264_write_scaling(v, 0xa0, p->scaling8[1], 16);
		scaling = 0xff << 10 | BIT(18);
	}
	word = 1 | 0x200000 | (p->log2_max_frame_num - 4) << 2 | (u32)p->poc_type << 6 |
	       (u32)p->max_ref_frames << 13 | BIT(18);
	if (!p->poc_type) word |= (p->log2_max_poc_lsb - 4) << 8;
	if (f & Y2_VDEC_H264_DELTA_POC_ZERO) word |= BIT(12);
	if (f & Y2_VDEC_H264_DIRECT_8X8) word |= BIT(20);
	vdec_write(v, VDEC_AVLD(0x88), word);
	vdec_write(v, VDEC_TOP(0x68), (v->mbs_w - 1) | (v->mbs_h - 1) << 16);
	word = (p->weighted_bipred_idc & 3) << 3 | (p->pic_init_qp & 0x3f) << 5 |
	       (p->chroma_qp_offset & 0x1f) << 11 | (p->second_chroma_qp_offset & 0x1f) << 19;
	if (f & Y2_VDEC_H264_CABAC) word |= BIT(0);
	if (f & Y2_VDEC_H264_PIC_ORDER_PRESENT) word |= BIT(1);
	if (f & Y2_VDEC_H264_WEIGHTED_PRED) word |= BIT(2);
	if (f & Y2_VDEC_H264_DEBLOCKING_CONTROL) word |= BIT(16);
	if (f & Y2_VDEC_H264_CONSTRAINED_INTRA) word |= BIT(17);
	if (f & Y2_VDEC_H264_TRANSFORM_8X8) word |= BIT(18);
	vdec_write(v, VDEC_AVLD(0x90), word);
	vdec_write(v, VDEC_AVLD(0x94), (p->ref_l0_default - 1) | (p->ref_l1_default - 1) << 5 | scaling);
	vdec_write(v, VDEC_AVLD(0x8c),
		   (vdec_read(v, VDEC_AVLD(0x8c)) & 0xfffe0000) | (s->first_mb & 0x1fff));
	vdec_write(v, VDEC_AVLD(0x98),
		   (vdec_read(v, VDEC_AVLD(0x98)) & 0x1fff) | (s->type & 0xf) << 13 |
		   s->direct_spatial << 19 | (s->l0 & 0x1f) << 20 | (s->l1 & 0x1f) << 25);
}

/* Vdec_Drv_setDecPicInfo. */
static void h264_write_picture(struct y2_vdec *v, const struct y2_vdec_h264 *p,
			       const struct h264_slice *s)
{
	struct h264_lists lists;
	h264_build_lists(p, &lists);
	h264_write_p_list(v, p, &lists);
	h264_write_b_lists(v, p, &lists);
	h264_write_parameters(v, p, s);
}

/* The marking syntax carries nothing the hardware needs; it is read to reach the fields behind it. */
static void h264_skip_marking(struct y2_vdec *v, const struct y2_vdec_h264 *p)
{
	unsigned n;
	if (p->nal_unit_type == 5) {
		h264_bits(v, 2);
		return;
	}
	if (!h264_bits(v, 1)) return;
	for (n = 0; n < 66; n++) {
		u32 op = h264_ue(v);
		if (!op) return;
		if (op == 1 || op == 2 || op == 4 || op == 6) h264_ue(v);
		if (op == 3) { h264_ue(v); h264_ue(v); }
	}
}

/* parseSliceHeaderHW for a frame of a frame_mbs_only stream. */
static int h264_slice_header(struct y2_vdec *v, const struct y2_vdec_h264 *p, struct h264_slice *s)
{
	u32 f = p->flags;
	*s = (struct h264_slice){ .l0 = p->ref_l0_default - 1, .l1 = p->ref_l1_default - 1 };
	s->first_mb = h264_ue(v);
	s->type = h264_ue(v);
	v->trace[5] = s->first_mb;
	v->trace[6] = s->type;
	v->trace[7] = vdec_read(v, VDEC_AVLD(0));
	if (s->type > 9) return -EILSEQ;
	s->kind = s->type % 5;
	h264_ue(v); /* pic_parameter_set_id */
	h264_bits(v, p->log2_max_frame_num);
	if (p->nal_unit_type == 5) h264_ue(v);
	if (p->poc_type == 0) {
		h264_bits(v, p->log2_max_poc_lsb);
		if (f & Y2_VDEC_H264_PIC_ORDER_PRESENT) h264_se(v);
	} else if (p->poc_type == 1 && !(f & Y2_VDEC_H264_DELTA_POC_ZERO)) {
		h264_se(v);
		if (f & Y2_VDEC_H264_PIC_ORDER_PRESENT) h264_se(v);
	}
	if (f & Y2_VDEC_H264_REDUNDANT_PIC_CNT) h264_ue(v);
	if (s->kind == SLICE_B) s->direct_spatial = h264_bits(v, 1);
	if (s->kind == SLICE_P || s->kind == SLICE_SP || s->kind == SLICE_B) {
		if (h264_bits(v, 1)) {
			s->l0 = h264_ue(v);
			if (s->kind == SLICE_B) s->l1 = h264_ue(v);
		}
		s->l0 = min(s->l0, 31U);
		s->l1 = min(s->l1, 31U);
	}
	if (s->kind != SLICE_B) s->l1 = 0;
	h264_write_picture(v, p, s);
	if (s->kind != SLICE_I && s->kind != SLICE_SI) {
		/* H264_HAL_Reording: the hardware reads the modification syntax and reorders. */
		u32 value;
		vdec_write(v, VDEC_AVLD(0x22c), 1);
		readl_poll_timeout_atomic(v->regs + VDEC_AVLD(0x22c), value, value, 1, 3000);
	}
	if (((f & Y2_VDEC_H264_WEIGHTED_PRED) && (s->kind == SLICE_P || s->kind == SLICE_SP)) ||
	    (p->weighted_bipred_idc == 1 && s->kind == SLICE_B)) {
		/* H264_HAL_PredWeightTable, write for write. */
		u32 value, misc = vdec_read(v, VDEC_MISC(0xf0)) | 1;
		vdec_write(v, VDEC_MISC(0xf0), misc);
		vdec_write(v, VDEC_AVLD(0x230), 1);
		readl_poll_timeout_atomic(v->regs + VDEC_AVLD(0x230), value, value, 1, 3000);
		vdec_write(v, VDEC_AVLD(0xf0), misc & ~1U);
	}
	if (p->nal_ref_idc) h264_skip_marking(v, p);
	if ((f & Y2_VDEC_H264_CABAC) && s->kind != SLICE_I && s->kind != SLICE_SI)
		s->cabac_init = h264_ue(v);
	s->qp_delta = h264_se(v);
	if (s->kind == SLICE_SP || s->kind == SLICE_SI) {
		if (s->kind == SLICE_SP) h264_bits(v, 1);
		h264_se(v);
	}
	if (f & Y2_VDEC_H264_DEBLOCKING_CONTROL) {
		s->deblock_idc = h264_ue(v);
		if (s->deblock_idc != 1) {
			s->alpha = h264_se(v);
			s->beta = h264_se(v);
		}
	}
	return 0;
}

/* H264_HAL_DecStart. */
static void h264_start(struct y2_vdec *v, const struct y2_vdec_h264 *p, const struct h264_slice *s)
{
	u32 y = v->frame[p->frame].dma, y_size = v->cfg.y_size, end = y + y_size * 3 / 2;
	u32 mv_low = ~0U, mv_high = 0, value;
	unsigned i;
	for (i = 0; i < v->cfg.frames; i++) {
		mv_low = min_t(u32, mv_low, v->mv[i].dma);
		mv_high = max_t(u32, mv_high, v->mv[i].dma);
	}
	vdec_write(v, VDEC_MISC(0x4), 3);
	vdec_write(v, VDEC_MC(0x90), 4);
	vdec_write(v, VDEC_TOP(0x58), ~0U);
	vdec_write(v, VDEC_MC(0x208), v->cfg.width);
	vdec_write(v, VDEC_MC(0x20c), v->cfg.height);
	vdec_write(v, VDEC_AVLD(0x280), H264_ERROR_MASK);
	vdec_write(v, VDEC_MC(0x220), 1);
	vdec_write(v, VDEC_MC(0x224), y >> 9);
	vdec_write(v, VDEC_MC(0x228), (y + y_size) >> 8);
	vdec_write(v, VDEC_MC(0x250), 1);
	vdec_write(v, VDEC_MC(0x238), 3);
	vdec_write(v, VDEC_MC(0x260), v->mbs_w - 1);
	vdec_write(v, VDEC_MC(0x264), v->mbs_h - 1);
	vdec_write(v, VDEC_MC(0x22c), v->mbs_w);
	vdec_write(v, VDEC_MC(0x850), end >> 4);
	vdec_write(v, VDEC_MC(0x854), y >> 4);
	vdec_write(v, VDEC_MC(0x858), 1);
	vdec_write(v, VDEC_MC(0x5e4), 0x20000);
	vdec_write(v, VDEC_MC(0x560), y);
	vdec_write(v, VDEC_MC(0x55c), y_size);
	vdec_write(v, VDEC_MC(0x980), v->mbs_w);
	vdec_write(v, VDEC_AVLD(0x2c8), 0xf9040039);
	vdec_write(v, VDEC_AMV(0x20c), v->mv[p->frame].dma >> 4);
	vdec_write(v, VDEC_AVLD(0x9c),
		   (s->qp_delta & 0x7f) | (s->deblock_idc & 3) << 7 | (s->alpha & 0xf) << 9 |
		   (s->beta & 0xf) << 13 | (u32)(p->nal_ref_idc & 3) << 17 |
		   (p->nal_unit_type == 5 ? BIT(19) : 0) | (s->cabac_init & 3) << 20);
	if (p->flags & Y2_VDEC_H264_CABAC) vdec_write(v, VDEC_AVLD(0x234), 1);
	readl_poll_timeout_atomic(v->regs + VDEC_AVLD(0x25c), value, !(value & BIT(25)), 1, 5000);
	vdec_set(v, VDEC_MC(0x5e4), 0x1000);
	vdec_write(v, VDEC_MC(0x768), (vdec_read(v, VDEC_MC(0x768)) & ~7U) | 4);
	vdec_set(v, VDEC_MISC(0xf0), 1);
	vdec_write(v, VDEC_TOP(0x6c), v->mbs_w * v->mbs_h);
	vdec_write(v, VDEC_TOP(0x50), ~0U);
	vdec_write(v, VDEC_MC(0xa20), v->work_b.dma);
	vdec_write(v, VDEC_MC(0xa24), v->work_a.dma);
	vdec_write(v, VDEC_AVLD(0x2d0), 0x103);
	vdec_write(v, VDEC_AVLD(0x2d4), 0xc210);
	vdec_set(v, VDEC_MISC(0x28), 1);
	vdec_write(v, VDEC_MISC(0x3c), y);
	vdec_write(v, VDEC_MISC(0x40), end);
	vdec_write(v, VDEC_MISC(0x54), mv_low);
	vdec_write(v, VDEC_MISC(0x58), mv_high + v->mv[0].size);
	vdec_write(v, VDEC_AVLD(0x84), H264_VLD_MODE);
	vdec_write(v, VDEC_AVLD(0x238), 1);
}

static int h264_valid(const struct y2_vdec *v, const struct y2_vdec_h264 *p)
{
	unsigned i;
	if (v->cfg.codec != Y2_VDEC_CODEC_H264 || p->frame >= v->cfg.frames || p->refs > 16)
		return -EINVAL;
	if (p->size < 5 || p->size > v->bits.size - VDEC_BITSTREAM_TAIL) return -EINVAL;
	if (p->log2_max_frame_num < 4 || p->log2_max_frame_num > 16 || p->poc_type > 2) return -EINVAL;
	if (!p->poc_type && (p->log2_max_poc_lsb < 4 || p->log2_max_poc_lsb > 16)) return -EINVAL;
	if (!p->ref_l0_default || p->ref_l0_default > 32 || !p->ref_l1_default || p->ref_l1_default > 32)
		return -EINVAL;
	for (i = 0; i < p->refs; i++)
		if (p->ref[i].frame >= v->cfg.frames) return -EINVAL;
	return 0;
}

int y2_vdec_h264_start(struct y2_vdec *v, const struct y2_vdec_h264 *p)
{
	struct h264_slice slice;
	unsigned attempt;
	int ret = h264_valid(v, p);
	if (ret) return ret;
	/* The stock library tries the reset and the feed up to five times. */
	for (attempt = 0; attempt < 5; attempt++) {
		h264_reset(v);
		ret = h264_feed(v, p->size);
		if (ret != -EIO) break;
	}
	if (ret) return ret;
	ret = h264_slice_header(v, p, &slice);
	if (ret) return ret;
	h264_start(v, p, &slice);
	return 0;
}

void y2_vdec_h264_result(struct y2_vdec *v, struct y2_vdec_h264 *p)
{
	/* H264_HAL_GetErrMsg reads 0x270 first; the error word is the latched one under the mask. */
	vdec_read(v, VDEC_AVLD(0x270));
	p->error = vdec_read(v, VDEC_AVLD(0x280)) & vdec_read(v, VDEC_AVLD(0x284));
	p->complete = vdec_read(v, VDEC_AVLD(0x278)) >> 31;
	p->mb_x = vdec_read(v, VDEC_MC(0x28));
	p->mb_y = vdec_read(v, VDEC_MC(0x2c));
	v->last_y = v->frame[p->frame].dma;
}
