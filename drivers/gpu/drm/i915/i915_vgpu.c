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

#include "i915_vgpu.h"

/**
 * DOC: Intel GVT-g guest support
 *
 * Intel GVT-g is a graphics virtualization technology which shares the
 * GPU among multiple virtual machines on a time-sharing basis. Each
 * virtual machine is presented a virtual GPU (vGPU), which has equivalent
 * features as the underlying physical GPU (pGPU), so i915 driver can run
 * seamlessly in a virtual machine. This file provides vGPU specific
 * optimizations when running in a virtual machine, to reduce the complexity
 * of vGPU emulation and to improve the overall performance.
 *
 * A primary function introduced here is so-called "address space ballooning"
 * technique. Intel GVT-g partitions global graphics memory among multiple VMs,
 * so each VM can directly access a portion of the memory without hypervisor's
 * intervention, e.g. filling textures or queuing commands. However with the
 * partitioning an unmodified i915 driver would assume a smaller graphics
 * memory starting from address ZERO, then requires vGPU emulation module to
 * translate the graphics address between 'guest view' and 'host view', for
 * all registers and command opcodes which contain a graphics memory address.
 * To reduce the complexity, Intel GVT-g introduces "address space ballooning",
 * by telling the exact partitioning knowledge to each guest i915 driver, which
 * then reserves and prevents non-allocated portions from allocation. Thus vGPU
 * emulation module only needs to scan and validate graphics addresses without
 * complexity of address translation.
 *
 */

/**
 * i915_detect_vgpu - detect virtual GPU
 * @dev_priv: i915 device private
 *
 * This function is called at the initialization stage, to detect whether
 * running on a vGPU.
 */
void intel_detect_vgpu(struct drm_i915_private *dev_priv)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	u64 magic;
	u16 version_major;
	void __iomem *shared_area;

	BUILD_BUG_ON(sizeof(struct vgt_if) != VGT_PVINFO_SIZE);

	/*
	 * This is called before we setup the main MMIO BAR mappings used via
	 * the uncore structure, so we need to access the BAR directly. Since
	 * we do not support VGT on older gens, return early so we don't have
	 * to consider differently numbered or sized MMIO bars
	 */
	if (INTEL_GEN(dev_priv) < 6)
		return;

	shared_area = pci_iomap_range(pdev, 0, VGT_PVINFO_PAGE, VGT_PVINFO_SIZE);
	if (!shared_area) {
		DRM_ERROR("failed to map MMIO bar to check for VGT\n");
		return;
	}

	magic = readq(shared_area + vgtif_offset(magic));
	if (magic != VGT_MAGIC)
		goto out;

	version_major = readw(shared_area + vgtif_offset(version_major));
	if (version_major < VGT_VERSION_MAJOR) {
		DRM_INFO("VGT interface version mismatch!\n");
		goto out;
	}

	dev_priv->vgpu.caps = readl(shared_area + vgtif_offset(vgt_caps));

	dev_priv->vgpu.active = true;
	mutex_init(&dev_priv->vgpu.lock);

	/* guest driver PV capability */
	dev_priv->vgpu.pv_caps = PV_PPGTT | PV_GGTT;
	dev_priv->vgpu.pv_caps |= PV_SUBMISSION | PV_HW_CONTEXT;
	dev_priv->vgpu.pv_caps |= PV_INTERRUPT;

	if (!intel_vgpu_check_pv_caps(dev_priv, shared_area)) {
		DRM_INFO("Virtual GPU for Intel GVT-g detected.\n");
		goto out;
	}

	DRM_INFO("Virtual GPU for Intel GVT-g detected with PV Optimized.\n");

out:
	pci_iounmap(pdev, shared_area);
}

void intel_destroy_vgpu(struct drm_i915_private *dev_priv)
{
       struct i915_virtual_gpu_pv *pv = dev_priv->vgpu.pv;

       if (!intel_vgpu_active(dev_priv) || !pv)
               return;

       __free_page(virt_to_page(pv->shared_page));
       kfree(pv);
}

bool intel_vgpu_has_full_ppgtt(struct drm_i915_private *dev_priv)
{
	return dev_priv->vgpu.caps & VGT_CAPS_FULL_PPGTT;
}

bool intel_vgpu_has_pv_caps(struct drm_i915_private *dev_priv)
{
	return dev_priv->vgpu.caps & VGT_CAPS_PV;
}

static void intel_vgpu_pv_notify(struct drm_i915_private *dev_priv)
{
       dev_priv->vgpu.pv->notify(dev_priv);
}

static bool intel_vgpu_enabled_pv_caps(struct drm_i915_private *dev_priv,
               enum pv_caps cap)
{
       return (dev_priv->vgpu.active && (dev_priv->vgpu.caps & VGT_CAPS_PV)
                       && (dev_priv->vgpu.pv_caps & cap));
}

static int intel_vgpu_pv_send(struct drm_i915_private *dev_priv,
               u32 *action, u32 len)
{
       return dev_priv->vgpu.pv->send(dev_priv, action, len);
}

struct _balloon_info_ {
	/*
	 * There are up to 2 regions per mappable/unmappable graphic
	 * memory that might be ballooned. Here, index 0/1 is for mappable
	 * graphic memory, 2/3 for unmappable graphic memory.
	 */
	struct drm_mm_node space[4];
};

static struct _balloon_info_ bl_info;

static void vgt_deballoon_space(struct i915_ggtt *ggtt,
				struct drm_mm_node *node)
{
	if (!drm_mm_node_allocated(node))
		return;

	DRM_DEBUG_DRIVER("deballoon space: range [0x%llx - 0x%llx] %llu KiB.\n",
			 node->start,
			 node->start + node->size,
			 node->size / 1024);

	ggtt->vm.reserved -= node->size;
	drm_mm_remove_node(node);
}

/**
 * intel_vgt_deballoon - deballoon reserved graphics address trunks
 * @ggtt: the global GGTT from which we reserved earlier
 *
 * This function is called to deallocate the ballooned-out graphic memory, when
 * driver is unloaded or when ballooning fails.
 */
void intel_vgt_deballoon(struct i915_ggtt *ggtt)
{
	int i;

	if (!intel_vgpu_active(ggtt->vm.i915))
		return;

	DRM_DEBUG("VGT deballoon.\n");

	for (i = 0; i < 4; i++)
		vgt_deballoon_space(ggtt, &bl_info.space[i]);
}

static int vgt_balloon_space(struct i915_ggtt *ggtt,
			     struct drm_mm_node *node,
			     unsigned long start, unsigned long end)
{
	unsigned long size = end - start;
	int ret;

	if (start >= end)
		return -EINVAL;

	DRM_INFO("balloon space: range [ 0x%lx - 0x%lx ] %lu KiB.\n",
		 start, end, size / 1024);
	ret = i915_gem_gtt_reserve(&ggtt->vm, node,
				   size, start, I915_COLOR_UNEVICTABLE,
				   0);
	if (!ret)
		ggtt->vm.reserved += size;

	return ret;
}

/**
 * intel_vgt_balloon - balloon out reserved graphics address trunks
 * @ggtt: the global GGTT from which to reserve
 *
 * This function is called at the initialization stage, to balloon out the
 * graphic address space allocated to other vGPUs, by marking these spaces as
 * reserved. The ballooning related knowledge(starting address and size of
 * the mappable/unmappable graphic memory) is described in the vgt_if structure
 * in a reserved mmio range.
 *
 * To give an example, the drawing below depicts one typical scenario after
 * ballooning. Here the vGPU1 has 2 pieces of graphic address spaces ballooned
 * out each for the mappable and the non-mappable part. From the vGPU1 point of
 * view, the total size is the same as the physical one, with the start address
 * of its graphic space being zero. Yet there are some portions ballooned out(
 * the shadow part, which are marked as reserved by drm allocator). From the
 * host point of view, the graphic address space is partitioned by multiple
 * vGPUs in different VMs. ::
 *
 *                         vGPU1 view         Host view
 *              0 ------> +-----------+     +-----------+
 *                ^       |###########|     |   vGPU3   |
 *                |       |###########|     +-----------+
 *                |       |###########|     |   vGPU2   |
 *                |       +-----------+     +-----------+
 *         mappable GM    | available | ==> |   vGPU1   |
 *                |       +-----------+     +-----------+
 *                |       |###########|     |           |
 *                v       |###########|     |   Host    |
 *                +=======+===========+     +===========+
 *                ^       |###########|     |   vGPU3   |
 *                |       |###########|     +-----------+
 *                |       |###########|     |   vGPU2   |
 *                |       +-----------+     +-----------+
 *       unmappable GM    | available | ==> |   vGPU1   |
 *                |       +-----------+     +-----------+
 *                |       |###########|     |           |
 *                |       |###########|     |   Host    |
 *                v       |###########|     |           |
 *  total GM size ------> +-----------+     +-----------+
 *
 * Returns:
 * zero on success, non-zero if configuration invalid or ballooning failed
 */
int intel_vgt_balloon(struct i915_ggtt *ggtt)
{
	struct intel_uncore *uncore = &ggtt->vm.i915->uncore;
	unsigned long ggtt_end = ggtt->vm.total;

	unsigned long mappable_base, mappable_size, mappable_end;
	unsigned long unmappable_base, unmappable_size, unmappable_end;
	int ret;

	if (!intel_vgpu_active(ggtt->vm.i915))
		return 0;

	mappable_base =
	  intel_uncore_read(uncore, vgtif_reg(avail_rs.mappable_gmadr.base));
	mappable_size =
	  intel_uncore_read(uncore, vgtif_reg(avail_rs.mappable_gmadr.size));
	unmappable_base =
	  intel_uncore_read(uncore, vgtif_reg(avail_rs.nonmappable_gmadr.base));
	unmappable_size =
	  intel_uncore_read(uncore, vgtif_reg(avail_rs.nonmappable_gmadr.size));

	mappable_end = mappable_base + mappable_size;
	unmappable_end = unmappable_base + unmappable_size;

	DRM_INFO("VGT ballooning configuration:\n");
	DRM_INFO("Mappable graphic memory: base 0x%lx size %ldKiB\n",
		 mappable_base, mappable_size / 1024);
	DRM_INFO("Unmappable graphic memory: base 0x%lx size %ldKiB\n",
		 unmappable_base, unmappable_size / 1024);

	if (mappable_end > ggtt->mappable_end ||
	    unmappable_base < ggtt->mappable_end ||
	    unmappable_end > ggtt_end) {
		DRM_ERROR("Invalid ballooning configuration!\n");
		return -EINVAL;
	}

	/* Unmappable graphic memory ballooning */
	if (unmappable_base > ggtt->mappable_end) {
		ret = vgt_balloon_space(ggtt, &bl_info.space[2],
					ggtt->mappable_end, unmappable_base);

		if (ret)
			goto err;
	}

	if (unmappable_end < ggtt_end) {
		ret = vgt_balloon_space(ggtt, &bl_info.space[3],
					unmappable_end, ggtt_end);
		if (ret)
			goto err_upon_mappable;
	}

	/* Mappable graphic memory ballooning */
	if (mappable_base) {
		ret = vgt_balloon_space(ggtt, &bl_info.space[0],
					0, mappable_base);

		if (ret)
			goto err_upon_unmappable;
	}

	if (mappable_end < ggtt->mappable_end) {
		ret = vgt_balloon_space(ggtt, &bl_info.space[1],
					mappable_end, ggtt->mappable_end);

		if (ret)
			goto err_below_mappable;
	}

	DRM_INFO("VGT balloon successfully\n");
	return 0;

err_below_mappable:
	vgt_deballoon_space(ggtt, &bl_info.space[0]);
err_upon_unmappable:
	vgt_deballoon_space(ggtt, &bl_info.space[3]);
err_upon_mappable:
	vgt_deballoon_space(ggtt, &bl_info.space[2]);
err:
	DRM_ERROR("VGT balloon fail\n");
	return ret;
}



static int vgpu_pv_vma_vm_action(struct drm_i915_private *dev_priv,
               u32 action, struct pv_vma *pvvma)
{
       u32 data[32];
       u32 size;

       size = sizeof(*pvvma) / 4;
       if (1 + size > ARRAY_SIZE(data))
               return -EIO;

       data[0] = action;
       memcpy(&data[1], pvvma, sizeof(*pvvma));
       return intel_vgpu_pv_send(dev_priv, data, 1 + size);
}

static int vgpu_pv_vma_action(struct i915_vma *vma,
               u32 action, u64 flags, u64 pte_flag)
{              
       struct drm_i915_private *i915 = vma->vm->i915;
       struct sgt_iter sgt_iter;
       dma_addr_t addr;
       struct pv_vma pvvma;
       u32 num_pages;
       u64 *gpas;
       int i = 0;
       u32 data[32];
       int ret;
       u32 size = sizeof(pvvma) / 4;

       if (1 + size > ARRAY_SIZE(data))
               return -EIO;

       num_pages = vma->node.size >> PAGE_SHIFT;
       pvvma.size = num_pages;
       pvvma.start = vma->node.start;
       pvvma.flags = flags;

       if (action == PV_ACTION_PPGTT_BIND ||
                       action == PV_ACTION_PPGTT_UNBIND ||
                       action == PV_ACTION_PPGTT_L4_INSERT)
               pvvma.pml4 = px_dma(i915_vm_to_ppgtt(vma->vm)->pd);

       if (num_pages == 1) {
               pvvma.dma_addrs = vma->pages->sgl->dma_address | pte_flag;
               goto out;
       }

       gpas = kmalloc_array(num_pages, sizeof(u64), GFP_KERNEL);
       if (gpas == NULL)
               return -ENOMEM;

       pvvma.dma_addrs = virt_to_phys((void *)gpas);
       for_each_sgt_daddr(addr, sgt_iter, vma->pages) {
               gpas[i++] = addr | pte_flag;
       }
       if (num_pages != i)
               pvvma.size = i;
out:
       data[0] = action;
       memcpy(&data[1], &pvvma, sizeof(pvvma));
       ret = intel_vgpu_pv_send(i915, data, 1 + size);

       if (num_pages > 1)
               kfree(gpas);

       return ret;
}

static void gen8_ppgtt_clear_pv(struct i915_address_space *vm,
               u64 start, u64 length)
{
       struct drm_i915_private *dev_priv = vm->i915;
       struct pv_vma ppgtt;

       ppgtt.pml4 = px_dma(i915_vm_to_ppgtt(vm)->pd);
       ppgtt.start = start;
       ppgtt.size = length >> PAGE_SHIFT;

       vgpu_pv_vma_vm_action(dev_priv, PV_ACTION_PPGTT_L4_CLEAR, &ppgtt);
}

static int gen8_ppgtt_alloc_pv(struct i915_address_space *vm,
               u64 start, u64 length)
{
       struct drm_i915_private *dev_priv = vm->i915;
       struct pv_vma ppgtt;
       u32 action = PV_ACTION_PPGTT_L4_ALLOC;

       ppgtt.pml4 = px_dma(i915_vm_to_ppgtt(vm)->pd);
       ppgtt.start = start;
       ppgtt.size = length >> PAGE_SHIFT;

       return vgpu_pv_vma_vm_action(dev_priv, action, &ppgtt);
}

static void gen8_ppgtt_insert_pv(struct i915_address_space *vm,
               struct i915_vma *vma,
               enum i915_cache_level cache_level, u32 flags)
{
       u64 pte_encode = vma->vm->pte_encode(0, cache_level, flags);

       vgpu_pv_vma_action(vma, PV_ACTION_PPGTT_L4_INSERT, 0, pte_encode);
}

static int ppgtt_bind_vma_pv(struct i915_vma *vma,
                         enum i915_cache_level cache_level,
                         u32 flags)
{
       u32 pte_flags;
       u64 pte_encode;

       if (flags & I915_VMA_ALLOC)
               set_bit(I915_VMA_ALLOC_BIT, __i915_vma_flags(vma));

       /* Applicable to VLV, and gen8+ */
       pte_flags = 0;
       if (i915_gem_object_is_readonly(vma->obj))
               pte_flags |= PTE_READ_ONLY;

       pte_encode = vma->vm->pte_encode(0, cache_level, pte_flags);

       GEM_BUG_ON(!test_bit(I915_VMA_ALLOC_BIT, __i915_vma_flags(vma)));

       vgpu_pv_vma_action(vma, PV_ACTION_PPGTT_BIND, flags, pte_encode);

       return 0;
}

static void ppgtt_unbind_vma_pv(struct i915_vma *vma)
{
       if (test_and_clear_bit(I915_VMA_ALLOC_BIT, __i915_vma_flags(vma)))
               vgpu_pv_vma_action(vma, PV_ACTION_PPGTT_UNBIND, 0, 0);
}

static void gen8_ggtt_insert_entries_pv(struct i915_address_space *vm,
               struct i915_vma *vma, enum i915_cache_level level, u32 flags)
{
       const gen8_pte_t pte_encode = vm->pte_encode(0, level, flags);

       vgpu_pv_vma_action(vma, PV_ACTION_GGTT_INSERT, 0, pte_encode);
}

static int ggtt_bind_vma_pv(struct i915_vma *vma,
		enum i915_cache_level cache_level, u32 flags)
{
	int ret;
	struct drm_i915_gem_object *obj = vma->obj;
	u32 pte_flags;

	/* Applicable to VLV (gen8+ do not support RO in the GGTT) */
	pte_flags = 0;
	if (i915_gem_object_is_readonly(obj))
		pte_flags |= PTE_READ_ONLY;

	pte_flags = vma->vm->pte_encode(0, cache_level, flags);
	ret = vgpu_pv_vma_action(vma, PV_ACTION_GGTT_BIND, 0, pte_flags);
	vma->page_sizes.gtt = I915_GTT_PAGE_SIZE;

	/*
	 * Without aliasing PPGTT there's no difference between
	 * GLOBAL/LOCAL_BIND, it's all the same ptes. Hence unconditionally
	 * upgrade to both bound if we bind either to avoid double-binding.
	 */
	atomic_or(I915_VMA_GLOBAL_BIND | I915_VMA_LOCAL_BIND, &vma->flags);

	return 0;
}

static void ggtt_unbind_vma_pv(struct i915_vma *vma)
{
	vgpu_pv_vma_action(vma, PV_ACTION_GGTT_UNBIND, 0, 0);
}

int vgpu_hwctx_pv_update(struct intel_context *ce, u32 action)
{
	struct drm_i915_private *i915 = ce->engine->i915;
	struct pv_hwctx pv_ctx;

	u32 data[32];
	int ret;
	u32 size = sizeof(pv_ctx) / 4;

	if (1 + size > ARRAY_SIZE(data))
		return -EIO;

	pv_ctx.ctx_gpa = virt_to_phys(ce);
	pv_ctx.eng_id = ce->engine->id;
	data[0] = action;
	memcpy(&data[1], &pv_ctx, sizeof(pv_ctx));
	ret = intel_vgpu_pv_send(i915, data, 1 + size);

	return ret;
}

/*
 * config guest driver PV ops for different PV features
 */
void intel_vgpu_config_pv_caps(struct drm_i915_private *dev_priv,
		enum pv_caps cap, void *data)
{
	struct i915_ppgtt *ppgtt;
	struct i915_ggtt *ggtt;
	struct intel_engine_cs *engine;

	if (!intel_vgpu_enabled_pv_caps(dev_priv, cap))
		return;

	if (cap == PV_PPGTT) {
		ppgtt = (struct i915_ppgtt *)data;
		ppgtt->vm.allocate_va_range = gen8_ppgtt_alloc_pv;
		ppgtt->vm.insert_entries = gen8_ppgtt_insert_pv;
		ppgtt->vm.clear_range = gen8_ppgtt_clear_pv;

		ppgtt->vm.vma_ops.bind_vma    = ppgtt_bind_vma_pv;
		ppgtt->vm.vma_ops.unbind_vma  = ppgtt_unbind_vma_pv;
	}

	if (cap == PV_GGTT) {
		ggtt = (struct i915_ggtt *)data;
		ggtt->vm.insert_entries = gen8_ggtt_insert_entries_pv;
		ggtt->vm.vma_ops.bind_vma    = ggtt_bind_vma_pv;
		ggtt->vm.vma_ops.unbind_vma  = ggtt_unbind_vma_pv;
	}

	if (cap == PV_SUBMISSION) {
		engine = (struct intel_engine_cs *)data;
		vgpu_set_pv_submission(engine);
	}

	if (cap == PV_HW_CONTEXT) {
		engine = (struct intel_engine_cs *)data;
		vgpu_engine_set_pv_context_ops(engine);
	}
}

/**
 * wait_for_desc_update - Wait for the command buffer descriptor update.
 * @desc:      buffer descriptor
 * @fence:     response fence
 * @status:    placeholder for status
 *
 * GVT will update command buffer descriptor with new fence and status
 * after processing the command identified by the fence. Wait for
 * specified fence and then read from the descriptor status of the
 * command.
 *
 * Return:
 * *   0 response received (status is valid)
 * *   -ETIMEDOUT no response within hardcoded timeout
 * *   -EPROTO no response, CT buffer is in error
 */
static int wait_for_desc_update(struct vgpu_pv_ct_buffer_desc *desc,
               u32 fence, u32 *status)
{
       int err;

#define done (READ_ONCE(desc->fence) == fence)
       err = wait_for_us(done, 5);
       if (err)
               err = wait_for(done, 10);
#undef done

       if (unlikely(err)) {
               DRM_ERROR("CT: fence %u failed; reported fence=%u\n",
                               fence, desc->fence);
       }

       *status = desc->status;
       return err;
}

/**
 * CTB Guest to GVT request
 *
 * Format of the CTB Guest to GVT request message is as follows::
 *
 *      +------------+---------+---------+---------+---------+
 *      |   msg[0]   |   [1]   |   [2]   |   ...   |  [n-1]  |
 *      +------------+---------+---------+---------+---------+
 *      |   MESSAGE  |       MESSAGE PAYLOAD                 |
 *      +   HEADER   +---------+---------+---------+---------+
 *      |            |    0    |    1    |   ...   |    n    |
 *      +============+=========+=========+=========+=========+
 *      |  len >= 1  |  FENCE  |     request specific data   |
 *      +------+-----+---------+---------+---------+---------+
 *
 *                   ^-----------------len-------------------^
 */
static int pv_command_buffer_write(struct i915_virtual_gpu_pv *pv,
               const u32 *action, u32 len /* in dwords */, u32 fence)
{
       struct vgpu_pv_ct_buffer_desc *desc = pv->ctb.desc;
       u32 head = desc->head / 4;      /* in dwords */
       u32 tail = desc->tail / 4;      /* in dwords */
       u32 size = desc->size / 4;      /* in dwords */
       u32 used;                       /* in dwords */
       u32 header;
       u32 *cmds = pv->ctb.cmds;
       unsigned int i;

       GEM_BUG_ON(desc->size % 4);
       GEM_BUG_ON(desc->head % 4);
       GEM_BUG_ON(desc->tail % 4);
       GEM_BUG_ON(tail >= size);

       /*
        * tail == head condition indicates empty.
        */
       if (tail < head)
               used = (size - head) + tail;
       else
               used = tail - head;

       /* make sure there is a space including extra dw for the fence */
       if (unlikely(used + len + 1 >= size))
               return -ENOSPC;

       /*
        * Write the message. The format is the following:
        * DW0: header (including action code)
        * DW1: fence
        * DW2+: action data
        */
       header = (len << PV_CT_MSG_LEN_SHIFT) |
                (PV_CT_MSG_WRITE_FENCE_TO_DESC) |
                (action[0] << PV_CT_MSG_ACTION_SHIFT);

       cmds[tail] = header;
       tail = (tail + 1) % size;

       cmds[tail] = fence;
       tail = (tail + 1) % size;

       for (i = 1; i < len; i++) {
               cmds[tail] = action[i];
               tail = (tail + 1) % size;
       }

       /* now update desc tail (back in bytes) */
       desc->tail = tail * 4;
       GEM_BUG_ON(desc->tail > desc->size);

       return 0;
}

static u32 pv_get_next_fence(struct i915_virtual_gpu_pv *pv)
{
	/* For now it's trivial */
	return ++pv->next_fence;
}

static int pv_send(struct drm_i915_private *dev_priv,
		const u32 *action, u32 len, u32 *status)
{
	struct i915_virtual_gpu *vgpu = &dev_priv->vgpu;
	struct i915_virtual_gpu_pv *pv = vgpu->pv;

	struct vgpu_pv_ct_buffer_desc *desc = pv->ctb.desc;

	u32 fence;
	int err;

	GEM_BUG_ON(!pv->enabled);
	GEM_BUG_ON(!len);
	GEM_BUG_ON(len & ~PV_CT_MSG_LEN_MASK);

	fence = pv_get_next_fence(pv);
	err = pv_command_buffer_write(pv, action, len, fence);
	if (unlikely(err))
		goto unlink;

	intel_vgpu_pv_notify(dev_priv);

	err = wait_for_desc_update(desc, fence, status);
	if (unlikely(err))
		goto unlink;

	if ((*status)) {
		err = -EIO;
		goto unlink;
	}

	err = (*status);
unlink:
	return err;
}

static int intel_vgpu_pv_send_command_buffer(
               struct drm_i915_private *dev_priv,
               u32 *action, u32 len)
{
       struct i915_virtual_gpu *vgpu = &dev_priv->vgpu;
       unsigned long flags;

       u32 status = ~0; /* undefined */
       int ret;

       spin_lock_irqsave(&vgpu->pv->lock, flags);

       ret = pv_send(dev_priv, action, len, &status);
       if (unlikely(ret < 0)) {
               DRM_ERROR("PV: send action %#X failed; err=%d status=%#X\n",
                         action[0], ret, status);
       } else if (unlikely(ret)) {
               DRM_ERROR("PV: send action %#x returned %d (%#x)\n",
                               action[0], ret, ret);
       }

       spin_unlock_irqrestore(&vgpu->pv->lock, flags);
       return ret;
}

static void intel_vgpu_pv_notify_mmio(struct drm_i915_private *dev_priv)
{
       I915_WRITE(vgtif_reg(g2v_notify), VGT_G2V_PV_SEND_TRIGGER);
}

/*
 * shared_page setup for VGPU PV features
 */
static int intel_vgpu_setup_shared_page(struct drm_i915_private *dev_priv,
		void __iomem *shared_area)
{
	void __iomem *addr;
	struct i915_virtual_gpu_pv *pv;
	struct gvt_shared_page *base;
	u64 gpa;
	u16 ver_maj, ver_min;
	int ret = 0;
	int i;
	u32 size;

	/* We allocate 1 page shared between guest and GVT for data exchange.
	 *       ___________.....................
	 *      |head       |                   |
	 *      |___________|.................. PAGE/8
	 *      |PV ELSP                        |
	 *      :___________....................PAGE/4
	 *      |desc (SEND)                    |
	 *      |				|
	 *      :_______________________________PAGE/2
	 *      |cmds (SEND)                    |
	 *      |                               |
	 *      |                               |
	 *      |                               |
	 *      |                               |
	 *      |_______________________________|
	 *
	 * 0 offset: PV version area
	 * PAGE/8 offset: per engine workload submission data area
	 * PAGE/4 offset: PV command buffer command descriptor area
	 * PAGE/2 offset: PV command buffer command data area
	 */

	base =  (struct gvt_shared_page *)get_zeroed_page(GFP_KERNEL);
	if (!base) {
		dev_info(dev_priv->drm.dev, "out of memory for shared memory\n");
		return -ENOMEM;
	}

	/* pass guest memory pa address to GVT and then read back to verify */
	gpa = __pa(base);
	addr = shared_area + vgtif_offset(shared_page_gpa);
	writeq(gpa, addr);
	if (gpa != readq(addr)) {
		dev_info(dev_priv->drm.dev, "passed shared_page_gpa failed\n");
		ret = -EIO;
		goto err;
	}

	addr = shared_area + vgtif_offset(g2v_notify);
	writel(VGT_G2V_SHARED_PAGE_SETUP, addr);

	ver_maj = base->ver_major;
	ver_min = base->ver_minor;
	if (ver_maj != PV_MAJOR || ver_min != PV_MINOR) {
		dev_info(dev_priv->drm.dev, "VGPU PV version incompatible\n");
		ret = -EIO;
		goto err;
	}

	pv = kzalloc(sizeof(struct i915_virtual_gpu_pv), GFP_KERNEL);
	if (!pv) {
		ret = -ENOMEM;
		goto err;
	}

	DRM_INFO("vgpu PV ver major %d and minor %d\n", ver_maj, ver_min);
	dev_priv->vgpu.pv = pv;
	pv->shared_page = base;
	pv->enabled = true;

	/* setup PV command buffer ptr */
	pv->ctb.cmds = (void *)base + PV_CMD_OFF;
	pv->ctb.desc = (void *)base + PV_DESC_OFF;
	pv->ctb.desc->size = PAGE_SIZE/2;
	pv->ctb.desc->addr = PV_CMD_OFF;

	/* setup PV command buffer callback */
	pv->send = intel_vgpu_pv_send_command_buffer;
	pv->notify = intel_vgpu_pv_notify_mmio;
	spin_lock_init(&pv->lock);

	/* setup PV per engine data exchange structure */
	size = sizeof(struct pv_submission);
	for (i = 0; i < I915_NUM_ENGINES; i++) {
		pv->pv_elsp[i] = (void *)base + PV_ELSP_OFF +  size * i;
		pv->pv_elsp[i]->submitted = false;
		spin_lock_init(&pv->pv_elsp[i]->lock);
	}

	/* setup PV irq data area */
	pv->irq = (void *)base + PV_INTERRUPT_OFF;

	return ret;
err:
	__free_page(virt_to_page(base));
	return ret;
}


/*
 * i915 vgpu PV support for Linux
 */

/**
 * intel_vgpu_check_pv_caps - detect virtual GPU PV capabilities
 * @dev_priv: i915 device private
 *
 * This function is called at the initialization stage, to detect VGPU
 * PV capabilities
 *
 * If guest wants to enable pv_caps, it needs to config it explicitly
 * through vgt_if interface from gvt layer.
 */
bool intel_vgpu_check_pv_caps(struct drm_i915_private *dev_priv,
		void __iomem *shared_area)
{
	u32 gvt_pvcaps;
	u32 pvcaps = 0;

	if (!intel_vgpu_has_pv_caps(dev_priv))
		return false;

	/* PV capability negotiation between PV guest and GVT */
	gvt_pvcaps = readl(shared_area + vgtif_offset(pv_caps));
	pvcaps = dev_priv->vgpu.pv_caps & gvt_pvcaps;
	dev_priv->vgpu.pv_caps = pvcaps;
	writel(pvcaps, shared_area + vgtif_offset(pv_caps));

	if (!pvcaps)
		return false;

	if (intel_vgpu_setup_shared_page(dev_priv, shared_area)) {
		dev_priv->vgpu.pv_caps = 0;
		writel(0, shared_area + vgtif_offset(pv_caps));
		return false;
	}

	return true;
}
