/***************************************************************
 *
 * (C) 2011-16 Nicola Bonelli <nicola@pfq.io>
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

#include <core/percpu.h>
#include <core/global.h>

#include <pfq/memory.h>
#include <pfq/percpu.h>
#include <pfq/pool.h>
#include <pfq/printk.h>


/* private */

static
size_t
pfq_skb_pool_flush(pfq_skb_pool_t *pool)
{
	size_t total = 0;
	struct sk_buff *skb;
	while ((skb = pfq_skb_pool_pop(pool)))
	{
		sparse_inc(global->percpu_mem_stats, os_free);
		kfree_skb(skb);
		total++;
	}
	return total;
}


static
int pfq_skb_pool_init (pfq_skb_pool_t **pool, size_t size, int cpu)
{
	int total = 0;
	if (!*pool) {

		struct sk_buff *skb;

		*pool = core_spsc_init(size);
		if (!*pool) {
			printk(KERN_ERR "[PFQ] pfq_skb_pool_init: out of memory!\n");
			return -ENOMEM;
		}

		for(; total < size; total++)
		{
			skb = __alloc_skb(PFQ_SKB_DEFAULT_SIZE, GFP_KERNEL, 1, cpu_to_node(cpu));
			if (!skb) {
				return total;
			}

			skb->pkt_type = PACKET_USER;
			pfq_skb_pool_push(*pool, skb);
			sparse_inc(global->percpu_mem_stats, os_alloc);
		}
	}

	return size;
}



size_t pfq_skb_pool_free(pfq_skb_pool_t **pool)
{
	size_t total = 0;
	if (*pool) {
		total = pfq_skb_pool_flush(*pool);
		kfree(*pool);
		*pool = NULL;
	}
	return total;
}


struct core_pool_stat
pfq_get_skb_pool_stats(void)
{
        struct core_pool_stat ret =
        {
           .os_alloc      = sparse_read(global->percpu_mem_stats, os_alloc)
        ,  .os_free       = sparse_read(global->percpu_mem_stats, os_free)

        ,  .pool_push     = sparse_read(global->percpu_mem_stats, pool_push)
        ,  .pool_pop      = sparse_read(global->percpu_mem_stats, pool_pop)
        ,  .pool_empty    = sparse_read(global->percpu_mem_stats, pool_empty)
        ,  .pool_norecycl = sparse_read(global->percpu_mem_stats, pool_norecycl)

        ,  .err_shared    = sparse_read(global->percpu_mem_stats, err_shared)
        ,  .err_cloned    = sparse_read(global->percpu_mem_stats, err_cloned)
        ,  .err_memory    = sparse_read(global->percpu_mem_stats, err_memory)
        ,  .err_irqdis    = sparse_read(global->percpu_mem_stats, err_irqdis)
        ,  .err_nolinr    = sparse_read(global->percpu_mem_stats, err_nolinr)
        ,  .err_fclone    = sparse_read(global->percpu_mem_stats, err_fclone)
	};
	return ret;
}

/* public */

int pfq_skb_pool_init_all(void)
{
	int cpu, total = 0;
	for_each_present_cpu(cpu)
	{
		struct pfq_percpu_pool *pool = per_cpu_ptr(global->percpu_pool, cpu);
		if (pool) {
                        int n;

			spin_lock_init(&pool->tx_pool_lock);

			if ((n = pfq_skb_pool_init(&pool->tx_pool, global->skb_pool_size, cpu)) < 0)
				return -ENOMEM;
			total += n;

			if ((n = pfq_skb_pool_init(&pool->rx_pool, global->skb_pool_size, cpu)) < 0)
				return -ENOMEM;
			total += n;
		}
	}

	printk(KERN_INFO "[PFQ] %d sk_buff allocated!\n", total);

	return 0;
}


int pfq_skb_pool_free_all(void)
{
	int cpu, total = 0;

	for_each_present_cpu(cpu)
	{
		struct pfq_percpu_pool *pool = per_cpu_ptr(global->percpu_pool, cpu);
		if (pool) {
			total += pfq_skb_pool_free(&pool->rx_pool);
			spin_lock(&pool->tx_pool_lock);
			total += pfq_skb_pool_free(&pool->tx_pool);
			spin_unlock(&pool->tx_pool_lock);
		}
	}

	printk(KERN_INFO "[PFQ] %d sk_buff freed!\n", total);
	return total;
}


void pfq_skb_pool_toggle(bool value)
{
	int cpu;

	smp_wmb();
	for_each_present_cpu(cpu)
	{
		struct pfq_percpu_pool *pool = per_cpu_ptr(global->percpu_pool, cpu);
		if (pool)
			atomic_set(&pool->enable, value);
	}
	smp_wmb();
}

