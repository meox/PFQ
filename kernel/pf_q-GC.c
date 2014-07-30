/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola.bonelli@cnit.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/


#include <pf_q-GC.h>


void gc_reset(struct gc_data *gc)
{
	size_t n;
	for(n = 0; n < gc->pool.len; ++n)
	{
		gc_log_init(&gc->log[n]);
	}
	gc->pool.len = 0;
}


struct gc_buff
gc_make_buff(struct gc_data *gc, struct sk_buff *skb)
{
	struct gc_buff ret;

	if (gc->pool.len >= Q_GC_POOL_QUEUE_LEN) {
		ret.skb = NULL;
	}
	else {
		struct pfq_cb *cb = (struct pfq_cb *)skb->cb;
                cb->log = &gc->log[gc->pool.len];
		gc->pool.queue[gc->pool.len++].skb = skb;
		ret.skb = skb;
	}

	return ret;
}


struct gc_buff
gc_alloc_buff(struct gc_data *gc, size_t size)
{
	struct sk_buff *skb;
	struct gc_buff ret;

	if (gc->pool.len >= Q_GC_POOL_QUEUE_LEN) {
		ret.skb = NULL;
		return ret;
	}

	skb = alloc_skb(size, GFP_ATOMIC);
	if (skb == NULL) {
		ret.skb = NULL;
		return ret;
	}

	return gc_make_buff(gc, skb);
}


struct gc_buff
gc_copy_buff(struct gc_data *gc, struct gc_buff orig)
{
	struct sk_buff *skb;
	struct gc_buff ret;

	if (gc->pool.len >= Q_GC_POOL_QUEUE_LEN) {
		ret.skb = NULL;
		return ret;
	}

	skb = skb_copy(orig.skb, GFP_ATOMIC);
	if (skb == NULL) {
		ret.skb = NULL;
		return ret;
	}

	ret = gc_make_buff(gc, skb);
	if (ret.skb) {

		PFQ_CB(ret.skb)->group_mask = PFQ_CB(orig.skb)->group_mask;
		PFQ_CB(ret.skb)->direct     = PFQ_CB(orig.skb)->direct;
		PFQ_CB(ret.skb)->monad      = PFQ_CB(orig.skb)->monad;
	}

	return ret;
}

