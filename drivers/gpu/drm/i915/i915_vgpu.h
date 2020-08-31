/*
 * Copyright(c) 2011-2015 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _I915_VGPU_H_
#define _I915_VGPU_H_

#include "i915_drv.h"
#include "i915_pvinfo.h"

#include "gt/intel_engine_types.h"

#define PV_MAJOR               1
#define PV_MINOR               0
#define PV_MAX_ENGINES_NUM     (VECS1_HW + 1)
#define PV_INTERRUPT_OFF (PAGE_SIZE/256)
#define PV_ELSP_OFF            (PAGE_SIZE/8)
#define PV_DESC_OFF            (PAGE_SIZE/4)
#define PV_CMD_OFF             (PAGE_SIZE/2)

/* ISR */
#define VGPU_IRQ_STATUS 0x0
/* IIR */
#define VGPU_IRQ_SOURCE 0x80

/* display engine id: vblank and de_port interrupt */
#define DISPLAY_ENG_ID I915_NUM_ENGINES
enum intel_display_eng_id {
       DISP_PIPE_A = I915_NUM_ENGINES + PIPE_A,
       DISP_PIPE_B,
       DISP_PIPE_C,
       DISP_PIPE_D,
       DISP_DE_PORT,
};

/* for each pipe */
#define PIPE_VBLANK 0

/* for aux channel */
enum intel_display_aux_channel {
       DP_AUX_CHANNEL_A,
       DP_AUX_CHANNEL_B,
       DP_AUX_CHANNEL_C,
       DP_AUX_CHANNEL_D,
       DP_AUX_CHANNEL_MAX,
};

/*
 * define different capabilities of PV optimization
 */
enum pv_caps {
       PV_PPGTT = BIT(0),
       PV_GGTT = BIT(1),
       PV_SUBMISSION = BIT(2),
       PV_HW_CONTEXT = BIT(3),
       PV_INTERRUPT = BIT(4),
};

/* PV actions */
enum intel_vgpu_pv_action {
       PV_ACTION_DEFAULT = 0x0,
       PV_ACTION_PPGTT_L4_ALLOC,
       PV_ACTION_PPGTT_L4_CLEAR,
       PV_ACTION_PPGTT_L4_INSERT,
       PV_ACTION_PPGTT_BIND,
       PV_ACTION_PPGTT_UNBIND,
       PV_ACTION_GGTT_INSERT,
       PV_ACTION_GGTT_UNBIND,
       PV_ACTION_GGTT_BIND,
       PV_ACTION_ELSP_SUBMISSION,
       PV_ACTION_HWCTX_ALLOC,
       PV_ACTION_HWCTX_DESTROY,
       PV_ACTION_HWCTX_PIN,
       PV_ACTION_HWCTX_UNPIN,
       PV_ACTION_HWCTX_RESET,
};

/*
 * A shared page(4KB) between gvt and VM, could be allocated by guest driver
 * or a fixed location in PCI bar 0 region
 */
struct gvt_shared_page {
       u16 ver_major;
       u16 ver_minor;
};

/* PV virtual memory address for GGTT/PPGTT */
struct pv_vma {
       u32 size; /* num of pages */
       u32 flags; /* bind or unbind flags */
       u64 start; /* start of virtual address */
       u64 dma_addrs; /* BO's dma address list */
       u64 pml4; /* ppgtt handler */
} __packed;

/* PV workload submission */
struct pv_submission {
       u64 descs[EXECLIST_MAX_PORTS];
       /* guest logical context handler */
       u64 ctx_gpa[EXECLIST_MAX_PORTS];
       bool submitted;
} __packed;

/* PV engine logical context */
struct pv_hwctx {
       u32 eng_id;
       u64 ctx_gpa; /* guest logical context handler */
} __packed;

/*
 * Definition of the command transport message header (DW0)
 *
 * bit[4..0]   message len (in dwords)
 * bit[7..5]   reserved
 * bit[8]              write fence to desc
 * bit[9..11]  reserved
 * bit[31..16] action code
 */
#define PV_CT_MSG_LEN_SHIFT                            0
#define PV_CT_MSG_LEN_MASK                             0x1F
#define PV_CT_MSG_WRITE_FENCE_TO_DESC  (1 << 8)
#define PV_CT_MSG_ACTION_SHIFT                 16
#define PV_CT_MSG_ACTION_MASK                  0xFFFF

/* PV command transport buffer descriptor */
struct vgpu_pv_ct_buffer_desc {
       u32 addr;               /* gpa address */
       u32 size;               /* size in bytes */
       u32 head;               /* offset updated by GVT */
       u32 tail;               /* offset updated by owner */

       u32 fence;              /* fence updated by GVT */
       u32 status;             /* status updated by GVT */
} __packed;

/** PV single command transport buffer.
 *
 * A single command transport buffer consists of two parts, the header
 * record (command transport buffer descriptor) and the actual buffer which
 * holds the commands.
 *
 * @desc: pointer to the buffer descriptor
 * @cmds: pointer to the commands buffer
 */
struct vgpu_pv_ct_buffer {
       struct vgpu_pv_ct_buffer_desc *desc;
       u32 *cmds;
};

struct i915_virtual_gpu_pv {
       struct gvt_shared_page *shared_page;
       bool enabled;

       void *irq; /* pv irq base */

       /* per engine PV workload submission data */
	   struct pv_submission *pv_elsp[I915_NUM_ENGINES];

       /* PV command buffer support */
       struct vgpu_pv_ct_buffer ctb;
       u32 next_fence;

       /* To serialize the vgpu PV send actions */
       spinlock_t lock;

       /* VGPU's PV specific send function */
       int (*send)(struct drm_i915_private *dev_priv, u32 *data, u32 len);
       void (*notify)(struct drm_i915_private *dev_priv);
};

void intel_detect_vgpu(struct drm_i915_private *dev_priv);
void intel_destroy_vgpu(struct drm_i915_private *dev_priv);

bool intel_vgpu_has_full_ppgtt(struct drm_i915_private *dev_priv);

static inline bool
intel_vgpu_has_hwsp_emulation(struct drm_i915_private *dev_priv)
{
	return dev_priv->vgpu.caps & VGT_CAPS_HWSP_EMULATION;
}

static inline bool
intel_vgpu_has_huge_gtt(struct drm_i915_private *dev_priv)
{
	return dev_priv->vgpu.caps & VGT_CAPS_HUGE_GTT;
}
bool intel_vgpu_has_pv_caps(struct drm_i915_private *dev_priv);

int intel_vgt_balloon(struct i915_ggtt *ggtt);
void intel_vgt_deballoon(struct i915_ggtt *ggtt);
/* i915 vgpu pv related functions */
bool intel_vgpu_check_pv_caps(struct drm_i915_private *dev_priv,
               void __iomem *shared_area);
void intel_vgpu_config_pv_caps(struct drm_i915_private *dev_priv,
               enum pv_caps cap, void *data);
void vgpu_set_pv_submission(struct intel_engine_cs *engine);
int vgpu_hwctx_pv_update(struct intel_context *ce, u32 action);

#endif /* _I915_VGPU_H_ */
