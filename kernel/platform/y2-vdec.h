/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* /dev/y2-vdec: the MT6582 hardware video decoder of the Innioasis Y2.
 *
 * One process at a time opens the device, sizes the buffers (CONFIGURE), maps the bitstream buffer and
 * the frames, and asks for one picture per DECODE call. The caller parses the stream (parameter sets,
 * picture order, reference marking); the driver programs the hardware and returns when the picture is in
 * the frame. Userspace never sees a register or a physical address.
 *
 * Frames are NV12 in the decoder's block layout: luma in 16x32 blocks, chroma (CbCr interleaved) in 16x16
 * blocks, blocks in raster order, each block row-major. Luma starts at offset 0 of the frame mapping and
 * is `y_size` bytes; chroma follows and is `c_size` bytes. */
#ifndef Y2_VDEC_UAPI_H
#define Y2_VDEC_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define Y2_VDEC_MAX_FRAMES 24
#define Y2_VDEC_CODEC_H264 1

/* mmap offsets, in pages: the bitstream buffer (read/write) and frame `n` (read only). */
#define Y2_VDEC_MAP_BITSTREAM 0
#define Y2_VDEC_MAP_FRAME(n) (1 + (n))

struct y2_vdec_config {
	__u32 codec;          /* in: Y2_VDEC_CODEC_* */
	__u32 width, height;  /* in: coded size, multiples of 16 */
	__u32 frames;         /* in: number of frames, 1..Y2_VDEC_MAX_FRAMES */
	__u32 y_size, c_size; /* out: bytes of luma and of chroma in one frame */
	__u32 bitstream_size; /* out: bytes of the bitstream buffer */
	__u32 reserved;
};

#define Y2_VDEC_H264_REF_LONG 0x01
struct y2_vdec_h264_ref {
	__u8 frame;      /* index of the frame holding the reference */
	__u8 flags;      /* Y2_VDEC_H264_REF_* */
	__u16 number;    /* frame_num, or LongTermFrameIdx of a long-term reference */
	__s32 top_poc, bottom_poc;
};

#define Y2_VDEC_H264_CABAC 0x0001
#define Y2_VDEC_H264_PIC_ORDER_PRESENT 0x0002
#define Y2_VDEC_H264_WEIGHTED_PRED 0x0004
#define Y2_VDEC_H264_DEBLOCKING_CONTROL 0x0008
#define Y2_VDEC_H264_CONSTRAINED_INTRA 0x0010
#define Y2_VDEC_H264_TRANSFORM_8X8 0x0020
#define Y2_VDEC_H264_REDUNDANT_PIC_CNT 0x0040
#define Y2_VDEC_H264_SCALING 0x0080
#define Y2_VDEC_H264_DELTA_POC_ZERO 0x0100
#define Y2_VDEC_H264_DIRECT_8X8 0x0200

/* One progressive H.264 picture (frame_mbs_only streams, 4:2:0, 8 bit). The bitstream buffer holds the
 * picture's slice NAL units, each behind a 00 00 01 start code, `size` bytes in all. */
struct y2_vdec_h264 {
	__u32 size;
	__u8 frame;        /* frame to decode into */
	__u8 nal_ref_idc, nal_unit_type;
	__u8 refs;         /* entries used in `ref` */
	__u16 flags;       /* Y2_VDEC_H264_* */
	__u16 frame_num;
	__s32 top_poc, bottom_poc;
	/* Sequence parameter set. */
	__u8 log2_max_frame_num, poc_type, log2_max_poc_lsb, max_ref_frames;
	/* Picture parameter set. */
	__u8 weighted_bipred_idc, ref_l0_default, ref_l1_default; /* counts, not minus one */
	__s8 pic_init_qp, chroma_qp_offset, second_chroma_qp_offset; /* pic_init_qp: minus 26 */
	__u8 reserved[2];
	struct y2_vdec_h264_ref ref[16];
	__u8 scaling4[6][16], scaling8[2][64]; /* raster order; read with Y2_VDEC_H264_SCALING */
	/* Result. */
	__u32 error;       /* the hardware's error word, 0 when the picture is good */
	__u32 complete;    /* 1 when the hardware reports the whole picture */
	__u32 mb_x, mb_y;  /* macroblock the hardware stopped at */
};

#define Y2_VDEC_CONFIGURE _IOWR('y', 1, struct y2_vdec_config)
#define Y2_VDEC_H264_DECODE _IOWR('y', 2, struct y2_vdec_h264)
#endif
