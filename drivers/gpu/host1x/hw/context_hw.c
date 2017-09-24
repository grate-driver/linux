 /*
 * Copyright 2017 Dmitry Osipenko <digetx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>

#include "../context.h"

/*
 * In case of a hardware-assisted context store, a DMA transfer job
 * is scheduled, it would be triggered by a Host1x HW data transfer
 * request to the DMA engine.
 *
 * In case of a software context store, a channels syncpoint is incremented
 * by 1, triggering scheduled context-store syncpoint interrupt job.
 *
 * In both cases registers data is read from the channels output FIFO
 * that contains result of indirect registers reads. In the end store
 * worker increments syncpoint, unblocking CDMA.
 */
static int setup_context_store(struct host1x *host,
			       struct host1x_channel *ch,
			       struct host1x_context *ctx,
			       struct host1x_context **ret)
{
	struct host1x_context *stored_ctx = ch->recent_ctx;
	struct host1x_syncpt *sp = stored_ctx->sp;
	struct host1x_waitlist *waiter = NULL;
	bool sw_store = ctx->sw_store;
	u32 syncpt_id = sp->id;
	u32 syncval = 0;
	unsigned int i;

	if (sw_store) {
		waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
		if (!waiter)
			return -ENOMEM;
	}

	/*
	 * Lock Host1x module during of indirect reads to avoid
	 * tampering of reading address by other channel.
	 */
	host1x_cdma_push(&ch->cdma,
			 host1x_opcode_setclass(HOST1X_CLASS_HOST1X, 0, 0),
			 HOST1X_OPCODE_ACQUIRE_MLOCK(HOST1X_MODULE_HOST1X));

	if (sw_store) {
		syncval = host1x_syncpt_incr_max(sp, 1);

		/* increment syncpoint, triggering context store interrupt */
		host1x_cdma_push(&ch->cdma,
			host1x_opcode_nonincr(host1x_uclass_incr_syncpt_r(), 1),
			host1x_class_host_incr_syncpt(0, syncpt_id));
	}

	for (i = 0; i < stored_ctx->store_pushes; i++)
		host1x_cdma_push(&ch->cdma,
				 stored_ctx->store_data[i].word0,
				 stored_ctx->store_data[i].word1);

	/* wait for the store completion */
	host1x_cdma_push(&ch->cdma,
			 host1x_opcode_setclass(HOST1X_CLASS_HOST1X,
				host1x_uclass_wait_syncpt_r(), 1),
			 host1x_class_host_wait_syncpt(syncpt_id,
				host1x_syncpt_incr_max(sp, 1)));

	host1x_cdma_push(&ch->cdma,
			 host1x_opcode_setclass(HOST1X_CLASS_HOST1X, 0, 0),
			 HOST1X_OPCODE_RELEASE_MLOCK(HOST1X_MODULE_HOST1X));

	/*
	 * Schedule a context store interrupt. Note that it is important
	 * to do it after pushing context store gathers to avoid immediate
	 * interrupt trigger if CDMA is active now and registers reads aren't
	 * ready yet.
	 */
	if (sw_store)
		host1x_intr_add_action(host, syncpt_id, syncval,
				       HOST1X_INTR_ACTION_CONTEXT_STORE,
				       stored_ctx, waiter, NULL);

	/* avoid releasing of the stored context before this jobs completion */
	host1x_context_get(stored_ctx);

	/* stored context will be released on this jobs completion */
	*ret = stored_ctx;

	return 0;
}

/*
 * Prepend this job with a HW context restore gathers, first
 * restore of a newly created context resets HW registers state.
 */
static void setup_context_restore(struct host1x_channel *ch,
				  struct host1x_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->restore_pushes; i++)
		host1x_cdma_push(&ch->cdma,
				 ctx->restore_data[i].word0,
				 ctx->restore_data[i].word1);

	/* track recently scheduled channels context to switch to */
	host1x_context_update_recent(ch, ctx, false);
}

static int setup_context(struct host1x *host,
			 struct host1x_channel *ch,
			 struct host1x_context *ctx,
			 struct host1x_context **stored_ctx)
{
	if (ctx) {
		/* hold recently scheduled channels context */
		host1x_context_get_recent(ch);

		if (host1x_context_store_required(ctx)) {
			int err = setup_context_store(host, ch, ctx,
						      stored_ctx);
			if (err)
				return err;
		}

		host1x_context_put(ch->recent_ctx);

		if (host1x_context_restore_required(ctx))
			setup_context_restore(ch, ctx);
	}

	return 0;
}
