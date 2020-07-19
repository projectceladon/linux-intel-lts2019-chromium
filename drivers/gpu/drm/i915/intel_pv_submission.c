// SPDX-License-Identifier: MIT
/*
 * Copyright © 2018 Intel Corporation
 */

#include "i915_vgpu.h"
#include "gt/intel_lrc_reg.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_ring.h"
#include "i915_trace.h"

#define CTX_DESC_FORCE_RESTORE BIT_ULL(2)

static u64 execlists_update_context(struct i915_request *rq)
{
	struct intel_context *ce = rq->context;
	u64 desc = ce->lrc_desc;
	u32 tail, prev;

	tail = intel_ring_set_tail(rq->ring, rq->tail);
	prev = ce->lrc_reg_state[CTX_RING_TAIL];
	if (unlikely(intel_ring_direction(rq->ring, tail, prev) <= 0))
		desc |= CTX_DESC_FORCE_RESTORE;
	ce->lrc_reg_state[CTX_RING_TAIL] = tail;
	rq->tail = rq->wa_tail;
	ce->lrc_desc &= ~CTX_DESC_FORCE_RESTORE;
	return desc;
}

static inline struct i915_priolist *to_priolist(struct rb_node *rb)
{
	return rb_entry(rb, struct i915_priolist, node);
}

static void pv_submit(struct intel_engine_cs *engine,
	       struct i915_request **out,
	       struct i915_request **end)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;
	struct i915_virtual_gpu_pv *pv = engine->i915->vgpu.pv;
	struct pv_submission *pv_elsp = pv->pv_elsp[engine->id];
	struct i915_request *rq;
	int n, err;

	memset(pv_elsp->descs, 0, sizeof(pv_elsp->descs));
	n = 0;

	do {
		rq = *out++;
		pv_elsp->descs[n] = execlists_update_context(rq);
		pv_elsp->ctx_gpa[n] = virt_to_phys(rq->context);
		n++;
	} while (out != end);

	spin_lock(&pv_elsp->lock);
	pv_elsp->submitted = true;
	writel(PV_ACTION_ELSP_SUBMISSION, execlists->submit_reg);

#define done (READ_ONCE(pv_elsp->submitted) == false)
	err = wait_for_atomic_us(done, 1000);
#undef done
	spin_unlock(&pv_elsp->lock);

	if (unlikely(err))
		DRM_ERROR("PV (%s) workload submission failed\n", engine->name);

}

static struct i915_request *schedule_in(struct i915_request *rq, int idx)
{
	trace_i915_request_in(rq, idx);

	intel_gt_pm_get(rq->engine->gt);
	return i915_request_get(rq);
}

static void pv_dequeue(struct intel_engine_cs *engine)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;
	struct i915_request **first = execlists->inflight;
	struct i915_request ** const last_port = first + execlists->port_mask;
	struct i915_request *last = first[0];
	struct i915_request **port;
	struct rb_node *rb;
	bool submit = false;

	lockdep_assert_held(&engine->active.lock);

	if (last) {
		if (*++first)
			return;
		last = NULL;
	}

	port = first;
	while ((rb = rb_first_cached(&execlists->queue))) {
		struct i915_priolist *p = to_priolist(rb);
		struct i915_request *rq, *rn;
		int i;

		priolist_for_each_request_consume(rq, rn, p, i) {
			if (last && rq->context != last->context) {
				if (port == last_port)
					goto done;

				*port = schedule_in(last,
						    port - execlists->inflight);
				port++;
			}


			list_del_init(&rq->sched.link);
			__i915_request_submit(rq);
			submit = true;
			last = rq;
		}

		rb_erase_cached(&p->node, &execlists->queue);
		i915_priolist_free(p);
	}
done:
	execlists->queue_priority_hint =
		rb ? to_priolist(rb)->priority : INT_MIN;
	if (submit) {
		*port = schedule_in(last, port - execlists->inflight);
		*++port = NULL;
		pv_submit(engine, first, port);
	}
	execlists->active = execlists->inflight;
}

static void schedule_out(struct i915_request *rq)
{
	trace_i915_request_out(rq);

	intel_gt_pm_put(rq->engine->gt);
	i915_request_put(rq);
}

static void vgpu_pv_submission_tasklet(unsigned long data)
{
	struct intel_engine_cs * const engine = (struct intel_engine_cs *)data;
	struct intel_engine_execlists * const execlists = &engine->execlists;
	struct i915_request **port, *rq;
	unsigned long flags;
	struct i915_virtual_gpu_pv *pv = engine->i915->vgpu.pv;
	struct pv_submission *pv_elsp = pv->pv_elsp[engine->id];

	spin_lock_irqsave(&engine->active.lock, flags);

	for (port = execlists->inflight; (rq = *port); port++) {
		if (!i915_request_completed(rq))
			break;

		schedule_out(rq);
	}

	if (port != execlists->inflight) {
		int idx = port - execlists->inflight;
		int rem = ARRAY_SIZE(execlists->inflight) - idx;

		memmove(execlists->inflight, port, rem * sizeof(*port));
	}

	if (!pv_elsp->submitted)
		pv_dequeue(engine);

	spin_unlock_irqrestore(&engine->active.lock, flags);
}

static void pv_reset_prepare(struct intel_engine_cs *engine)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;

	GEM_TRACE("%s\n", engine->name);

	/*
	 * Prevent request submission to the hardware until we have
	 * completed the reset in i915_gem_reset_finish(). If a request
	 * is completed by one engine, it may then queue a request
	 * to a second via its execlists->tasklet *just* as we are
	 * calling engine->init_hw() and also writing the ELSP.
	 * Turning off the execlists->tasklet until the reset is over
	 * prevents the race.
	 */
	__tasklet_disable_sync_once(&execlists->tasklet);
}

static void
cancel_port_requests(struct intel_engine_execlists * const execlists)
{
	struct i915_request * const *port, *rq;

	/* Note we are only using the inflight and not the pending queue */
	for (port = execlists->active; (rq = *port); port++)
		schedule_out(rq);
	execlists->active =
		memset(execlists->inflight, 0, sizeof(execlists->inflight));
}

static void pv_reset_rewind(struct intel_engine_cs *engine, bool stalled)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;
	struct i915_request *rq;
	unsigned long flags;

	spin_lock_irqsave(&engine->active.lock, flags);

	cancel_port_requests(execlists);

	/* Push back any incomplete requests for replay after the reset. */
	rq = execlists_unwind_incomplete_requests(execlists);
	if (!rq)
		goto out_unlock;

	if (!i915_request_started(rq))
		stalled = false;

	__i915_request_reset(rq, stalled);
	intel_lr_context_reset(engine, rq->context, rq->head, stalled);

out_unlock:
	spin_unlock_irqrestore(&engine->active.lock, flags);
}

static void pv_reset_finish(struct intel_engine_cs *engine)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;

	if (__tasklet_enable(&execlists->tasklet))
		/* And kick in case we missed a new request submission. */
		tasklet_hi_schedule(&execlists->tasklet);

	GEM_TRACE("%s: depth->%d\n", engine->name,
		  atomic_read(&execlists->tasklet.count));
}

static void pv_cancel_requests(struct intel_engine_cs *engine)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;
	struct i915_request *rq, *rn;
	struct rb_node *rb;
	unsigned long flags;

	GEM_TRACE("%s\n", engine->name);

	spin_lock_irqsave(&engine->active.lock, flags);

	/* Cancel the requests on the HW and clear the ELSP tracker. */
	cancel_port_requests(execlists);

	/* Mark all executing requests as skipped. */
	list_for_each_entry(rq, &engine->active.requests, sched.link) {
		i915_request_skip(rq, -EIO);
		i915_request_mark_complete(rq);
	}

	/* Flush the queued requests to the timeline list (for retiring). */
	while ((rb = rb_first_cached(&execlists->queue))) {
		struct i915_priolist *p = to_priolist(rb);
		int i;

		priolist_for_each_request_consume(rq, rn, p, i) {
			list_del_init(&rq->sched.link);
			__i915_request_submit(rq);
			dma_fence_set_error(&rq->fence, -EIO);
			i915_request_mark_complete(rq);
		}

		rb_erase_cached(&p->node, &execlists->queue);
		i915_priolist_free(p);
	}

	execlists->queue_priority_hint = INT_MIN;
	execlists->queue = RB_ROOT_CACHED;

	spin_unlock_irqrestore(&engine->active.lock, flags);
}

void vgpu_set_pv_submission(struct intel_engine_cs *engine)
{
	/*
	 * We inherit a bunch of functions from execlists that we'd like
	 * to keep using:
	 *
	 *    engine->submit_request = execlists_submit_request;
	 *    engine->cancel_requests = execlists_cancel_requests;
	 *    engine->schedule = execlists_schedule;
	 *
	 * But we need to override the actual submission backend in order
	 * to talk to the GVT with PV notification message.
	 */

	engine->execlists.tasklet.func = vgpu_pv_submission_tasklet;

	/* do not use execlists park/unpark */
	engine->park = engine->unpark = NULL;

	engine->reset.prepare = pv_reset_prepare;
	engine->reset.rewind = pv_reset_rewind;
	engine->reset.cancel = pv_cancel_requests;
	engine->reset.finish = pv_reset_finish;

	engine->flags &= ~I915_ENGINE_SUPPORTS_STATS;
	engine->flags |= I915_ENGINE_NEEDS_BREADCRUMB_TASKLET;
}
