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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

#include <stdlib.h>

#include <sched.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <strings.h>

#include <linux/if_ether.h>
#include <linux/pf_q.h>

#include <pfq/pfq.h>
#include <pfq/pfq-int.h>

#include "cJSON.h"


/* macros */


#define Q_VALUE(q,value)  \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t *), (((pfq_t *)q)->error = NULL, (value)), \
      ( __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t const *), (((pfq_t *)q)->error = NULL, (value)), (void)0)))


#define Q_ERROR(q,msg)	 \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t *), (((pfq_t *)q)->error = (msg), -1), \
      ( __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t const *), (((pfq_t *)q)->error = (msg), -1), (void)0)))


#define Q_OK(q) Q_VALUE(q,0)


#define max(a,b) \
	({ __typeof__ (a) _a = (a); \
	   __typeof__ (b) _b = (b); \
	  _a > _b ? _a : _b; })

#define min(a,b) \
	({ __typeof__ (a) _a = (a); \
	   __typeof__ (b) _b = (b); \
	  _a < _b ? _a : _b; })


char *
pfq_hugepages_mountpoint()
{
	FILE *mp;
	char *line = NULL, *mount_point = NULL;
	size_t len = 0;
	ssize_t read;

	mp = fopen("/proc/mounts", "r");
	if (!mp)
		return NULL;

	while((read = getline(&line, &len, mp)) != -1) {
		char mbuff[256];
		if(sscanf(line, "hugetlbfs %s", mbuff) == 1) {
			mount_point = strdup(mbuff);
			break;
		}
	}

	free (line);
	fclose (mp);
	return mount_point;
}


/* return the string error */

static __thread const char * __error;

const char *pfq_string_version = PFQ_VERSION_STRING;


const char *pfq_error(pfq_t const *q)
{
        const char * p = q == NULL ? __error : q->error;
	return p == NULL ? "NULL" : p;
}


/* costructor */


pfq_t *
pfq_open(size_t caplen, size_t rx_slots, size_t xmitlen, size_t tx_slots)
{
	return pfq_open_group(Q_CLASS_DEFAULT, Q_POLICY_GROUP_PRIVATE, caplen, rx_slots, xmitlen, tx_slots);
}


pfq_t *
pfq_open_nogroup(size_t caplen, size_t rx_slots, size_t xmitlen, size_t tx_slots)
{
	return pfq_open_group(Q_CLASS_DEFAULT, Q_POLICY_GROUP_UNDEFINED, caplen, rx_slots, xmitlen, tx_slots);
}


pfq_t *
pfq_open_group(unsigned long class_mask, int group_policy, size_t caplen, size_t rx_slots, size_t xmitlen, size_t tx_slots)
{
	int fd = socket(PF_Q, SOCK_RAW, htons(ETH_P_ALL));
	pfq_t * q;

	if (fd == -1) {
		return __error = "PFQ: module not loaded", NULL;
	}

	q = (pfq_t *) malloc(sizeof(pfq_t));
	if (q == NULL) {
		return __error = "PFQ: out of memory", NULL;
	}

	/* initialize everything to 0 */

	memset(q, 0, sizeof(pfq_t));

	q->fd = fd;
	q->hd = -1;
	q->id = -1;
	q->gid = -1;

        memset(&q->nq, 0, sizeof(q->nq));

	/* get id */

	q->id = PFQ_VERSION_CODE;
	socklen_t size = sizeof(q->id);

	if (getsockopt(fd, PF_Q, Q_SO_GET_ID, &q->id, &size) == -1) {
		return __error = "PFQ: get id error", free(q), NULL;
	}

	/* set Rx queue slots */

	if (setsockopt(fd, PF_Q, Q_SO_SET_RX_SLOTS, &rx_slots, sizeof(rx_slots)) == -1) {
		return __error = "PFQ: set Rx slots error", free(q), NULL;
	}

	q->rx_slots = rx_slots;

	/* set caplen */

	if (setsockopt(fd, PF_Q, Q_SO_SET_RX_LEN, &caplen, sizeof(caplen)) == -1) {
		return __error = "PFQ: set Rx len error (caplen)", free(q), NULL;
	}

	q->rx_len = caplen;
	q->rx_slot_size = ALIGN(sizeof(struct pfq_pkthdr) + caplen, PFQ_SLOT_ALIGNMENT);

	/* set Tx queue slots */

	if (setsockopt(fd, PF_Q, Q_SO_SET_TX_SLOTS, &tx_slots, sizeof(tx_slots)) == -1) {
		return __error = "PFQ: set Tx slots error", free(q), NULL;
	}

	q->tx_slots = tx_slots;

        /* set xmitlen */

        if (setsockopt(fd, PF_Q, Q_SO_SET_TX_LEN, &xmitlen, sizeof(xmitlen)) == -1)
        {
		return __error = "PFQ: set Tx len error (xmitlen)", free(q), NULL;
        }

	q->tx_len = xmitlen;
	q->tx_slot_size = ALIGN(sizeof(struct pfq_pkthdr) + (size_t)xmitlen, PFQ_SLOT_ALIGNMENT);

	/* join group if specified */

	if (group_policy != Q_POLICY_GROUP_UNDEFINED)
	{
		q->gid = pfq_join_group(q, Q_ANY_GROUP, class_mask, group_policy);
		if (q->gid == -1) {
			return __error = q->error, free(q), NULL;
		}
	}

	return __error = NULL, q;
}


int pfq_close(pfq_t *q)
{
	if (q->fd != -1)
	{
		if (q->shm_addr)
			pfq_disable(q);

		if (close(q->fd) < 0)
			return Q_ERROR(q, "PFQ: close error");

		if (q->hd != -1)
			close(q->hd);

		free(q);
                return Q_OK(q);
	}

	free(q);
	return __error = "PFQ: socket not open", -1;
}



static unsigned long long
parse_size(const char *str)
{
    char *endptr;
    unsigned long long v = strtoull(str, &endptr, 10);
    switch(*endptr)
    {
    case '\0': return v;
    case 'k': case 'K':  return v * 1024;
    case 'm': case 'M':  return v * 1024 * 1024;
    case 'g': case 'G':  return v * 1024 * 1024 * 1024;
    }

    return 0;
}


static
size_t get_hugepage_size(size_t size)
{
#define HP_1G  (1048576*1024)
#define HP_16M (16384*1024)
#define HP_4M  (4096*1024)
#define HP_2M  (2048*1024)

	struct stat x;
	if (stat("/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages", &x) == 0) {
		if ((size%HP_1G) == 0)
			return HP_1G;
	}
	if (stat("/sys/kernel/mm/hugepages/hugepages-16384kB/nr_hugepages", &x) == 0) {
		if ((size%HP_16M) == 0)
			return HP_16M;
	}
	if (stat("/sys/kernel/mm/hugepages/hugepages-4096kB/nr_hugepages", &x) == 0) {
		if ((size%HP_4M) == 0)
			return HP_4M;
	}
	if (stat("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", &x) == 0) {
		return 2048*1024;
	}
	return 0;
}


static inline
const char *size_to_string(size_t size)
{
	switch(size)
	{
	case 2048*1024:		return "2M";
	case 4096*1024:		return "4M";
	case 16*1024*1024:	return "16M";
	case 1024*1024*1024:	return "1G";
	}
	return "UnknownSize";
}


int
pfq_enable(pfq_t *q)
{
	size_t sock_mem; socklen_t size = sizeof(sock_mem);
	char filename[256], *hugepages_mpoint;
        char *pfq_hugepages;

	struct pfq_so_enable mem = { .user_addr = 0
				   , .user_size  = 0
				   , .hugepage_size = 0
				   };

	if (q->shm_addr != MAP_FAILED &&
	    q->shm_addr != NULL) {
		return Q_ERROR(q, "PFQ: queue already enabled");
	}

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_SHMEM_SIZE, &sock_mem, &size) == -1) {
		return Q_ERROR(q, "PFQ: queue memory error");
	}

	pfq_hugepages = getenv("PFQ_HUGEPAGES");

	hugepages_mpoint = pfq_hugepages_mountpoint();

	if (hugepages_mpoint && pfq_hugepages) {
		mem.user_size = parse_size(pfq_hugepages);
	}

	if (mem.user_size > 0)
	{
		/* HugePages */

		mem.hugepage_size = get_hugepage_size(mem.user_size);
		if (!mem.hugepage_size) {
			return Q_ERROR(q, "PFQ: HugePages not enabled!");
		}

		fprintf(stdout, "[PFQ] using %s HugePages (%zu bytes)...\n",
			size_to_string(mem.hugepage_size),
			mem.user_size);

		snprintf(filename, 256, "%s/pfq.%d", hugepages_mpoint, getpid());
		free (hugepages_mpoint);

		/* open hugepage file */

		q->hd = open(filename, O_CREAT | O_RDWR, 0755);
		if (q->hd == -1)
			return Q_ERROR(q, "PFQ: couldn't open a HugePages");

		/* mmap HugePages */

		q->shm_hugepages = mmap(NULL, mem.user_size, PROT_READ|PROT_WRITE, MAP_SHARED, q->hd, 0);
		if (q->shm_hugepages == MAP_FAILED) {
			return Q_ERROR(q, "PFQ: HugePages");
		}

		q->shm_hugepages_size = mem.user_size;

		mem.user_addr = (unsigned long)q->shm_hugepages;

		/* enable socket memory */

		if(setsockopt(q->fd, PF_Q, Q_SO_ENABLE, &mem, sizeof(mem)) == -1)
			return Q_ERROR(q, "PFQ: socket enable (HugePages)");

		/* queue memory... */

		q->shm_addr = (void *)mem.user_addr;
		q->shm_size = mem.user_size;

		printf("hugepages = %p shm_addr = %p\n", q->shm_hugepages, q->shm_addr);
	}
	else {
		/* Standard pages (4K) */

		fprintf(stdout, "[PFQ] using 4k-Pages...\n");

		free (hugepages_mpoint);

		mem.user_addr = 0;
		mem.user_size = 0;
		mem.hugepage_size = 0;

		if(setsockopt(q->fd, PF_Q, Q_SO_ENABLE, &mem, sizeof(mem)) == -1)
			return Q_ERROR(q, "PFQ: socket enable");

		/* queue memory... */

		q->shm_addr = mmap(NULL, sock_mem, PROT_READ|PROT_WRITE, MAP_SHARED, q->fd, 0);
		if (q->shm_addr == MAP_FAILED)
			return Q_ERROR(q, "PFQ: socket enable (memory map)");

		q->shm_size = sock_mem;

		/* hugepages... */
		q->shm_hugepages = NULL;
		q->shm_hugepages_size = 0;
	}

	q->rx_queue_addr = (char *)(q->shm_addr) + sizeof(struct pfq_shared_queue);
	q->rx_queue_size = q->rx_slots * q->rx_slot_size;

	q->tx_queue_addr = (char *)(q->shm_addr) + sizeof(struct pfq_shared_queue) + q->rx_queue_size * 2;
	q->tx_queue_size = q->tx_slots * q->tx_slot_size;

	return Q_OK(q);
}


int
pfq_disable(pfq_t *q)
{
	if (q->fd == -1)
		return Q_ERROR(q, "PFQ: socket not open");

	if (q->shm_addr != MAP_FAILED) {

		if (q->shm_hugepages_size) {
			if (munmap(q->shm_hugepages, q->shm_hugepages_size) == -1)
				return Q_ERROR(q, "PFQ: munmap error");
		}
		else {
			if (munmap(q->shm_addr, q->shm_size) == -1)
				return Q_ERROR(q, "PFQ: munmap error");
		}
	}

	q->shm_addr = NULL;
	q->shm_size = 0;

	if(setsockopt(q->fd, PF_Q, Q_SO_DISABLE, NULL, 0) == -1) {
		return Q_ERROR(q, "PFQ: socket disable");
	}

	if (q->hd != -1) {
		char filename[256];
		char *hugepages = pfq_hugepages_mountpoint();
		if (hugepages) {
			snprintf(filename, 256, "%s/pfq.%d", hugepages, getpid());
			if (unlink(filename)) {
				fprintf(stdout, "[PFQ] coundn't unlink HugePages (%s)...\n", hugepages);
			}
			free(hugepages);
		}
	}

	return Q_OK(q);
}


int
pfq_is_enabled(pfq_t const *q)
{
	if (q->fd != -1)
	{
		int ret; socklen_t size = sizeof(ret);
		if (getsockopt(q->fd, PF_Q, Q_SO_GET_STATUS, &ret, &size) == -1) {
			return Q_ERROR(q, "PFQ: get status error");
		}
		return Q_VALUE(q, ret);
	}
	return Q_OK(q);
}


int
pfq_timestamping_enable(pfq_t *q, int value)
{
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_RX_TSTAMP, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set timestamp mode");
	}
	return Q_OK(q);
}


int
pfq_is_timestamping_enabled(pfq_t const *q)
{
	int ret; socklen_t size = sizeof(int);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_RX_TSTAMP, &ret, &size) == -1) {
	        return Q_ERROR(q, "PFQ: get timestamp mode");
	}
	return Q_VALUE(q, ret);
}


int
pfq_set_weight(pfq_t *q, int value)
{
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_WEIGHT, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set socket weight");
	}
	return Q_OK(q);
}


int
pfq_get_weight(pfq_t const *q)
{
	int ret; socklen_t size = sizeof(ret);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_WEIGHT, &ret, &size) == -1) {
	        return Q_ERROR(q, "PFQ: get socket weight");
	}
	return Q_VALUE(q, ret);
}

int
pfq_ifindex(pfq_t const *q, const char *dev)
{
	struct ifreq ifreq_io;

	memset(&ifreq_io, 0, sizeof(struct ifreq));
	strncpy(ifreq_io.ifr_name, dev, IFNAMSIZ);
	if (ioctl(q->fd, SIOCGIFINDEX, &ifreq_io) == -1) {
		return Q_ERROR(q, "PFQ: ioctl get ifindex error");
	}
	return Q_VALUE(q, ifreq_io.ifr_ifindex);
}


int
pfq_set_promisc(pfq_t const *q, const char *dev, int value)
{
	struct ifreq ifreq_io;

	memset(&ifreq_io, 0, sizeof(struct ifreq));
	strncpy(ifreq_io.ifr_name, dev, IFNAMSIZ);

	if(ioctl(q->fd, SIOCGIFFLAGS, &ifreq_io) == -1) {
		return Q_ERROR(q, "PFQ: ioctl getflags error");
	}

	if (value)
		ifreq_io.ifr_flags |= IFF_PROMISC;
	else
		ifreq_io.ifr_flags &= ~IFF_PROMISC;

	if(ioctl(q->fd, SIOCSIFFLAGS, &ifreq_io) == -1) {
		return Q_ERROR(q, "PFQ: ioctl setflags error");
	}
	return Q_OK(q);
}


int
pfq_set_caplen(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return Q_ERROR(q, "PFQ: enabled (caplen could not be set)");
	}

	if (setsockopt(q->fd, PF_Q, Q_SO_SET_RX_LEN, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set Rx len error (caplen)");
	}

	q->rx_len = value;
	q->rx_slot_size = ALIGN(sizeof(struct pfq_pkthdr) + value, PFQ_SLOT_ALIGNMENT);

	return Q_OK(q);
}


size_t
pfq_get_caplen(pfq_t const *q)
{
	return q->rx_len;
}


int
pfq_set_xmitlen(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return Q_ERROR(q, "PFQ: enabled (xmitlen could not be set)");
	}

	if (setsockopt(q->fd, PF_Q, Q_SO_SET_TX_LEN, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set xmitlen error");
	}

	q->tx_len = value;
	q->tx_slot_size = ALIGN(sizeof(struct pfq_pkthdr) + value, PFQ_SLOT_ALIGNMENT);

	return Q_OK(q);
}


size_t
pfq_get_xmitlen(pfq_t const *q)
{
	return q->tx_len;
}


size_t
pfq_get_rx_slots(pfq_t const *q)
{
	return q->rx_slots;
}



int
pfq_set_rx_slots(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return Q_ERROR(q, "PFQ: enabled (slots could not be set)");
	}
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_RX_SLOTS, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set Rx slots error");
	}

	q->rx_slots = value;
	return Q_OK(q);
}


int
pfq_set_tx_slots(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return Q_ERROR(q, "PFQ: enabled (Tx slots could not be set)");
	}
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_TX_SLOTS, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set Tx slots error");
	}

	q->tx_slots = value;
	return Q_OK(q);
}


size_t
pfq_get_tx_slots(pfq_t const *q)
{
	return q->tx_slots;
}


size_t
pfq_get_rx_slot_size(pfq_t const *q)
{
	return q->rx_slot_size;
}

size_t
pfq_get_tx_slot_size(pfq_t const *q)
{
	return q->tx_slot_size;
}


int
pfq_bind_group(pfq_t *q, int gid, const char *dev, int queue)
{
	struct pfq_so_binding b;
	int index;

	if (strcmp(dev, "any")==0) {
		index = Q_ANY_DEVICE;
	}
	else {
		index = pfq_ifindex(q, dev);
		if (index == -1) {
			return Q_ERROR(q, "PFQ: bind_group: device not found");
		}
	}

	b.gid     = gid;
	b.ifindex = index;
	b.qindex  = queue;

	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_BIND, &b, sizeof(b)) == -1) {
		return Q_ERROR(q, "PFQ: bind error");
	}
	return Q_OK(q);
}


int
pfq_bind(pfq_t *q, const char *dev, int queue)
{
	int gid = q->gid;
	if (gid < 0) {
		return Q_ERROR(q, "PFQ: default group undefined");
	}
	return pfq_bind_group(q, gid, dev, queue);
}


int
pfq_egress_bind(pfq_t *q, const char *dev, int queue)
{
	struct pfq_so_binding b;

	int index;
	if (strcmp(dev, "any")==0) {
		index = Q_ANY_DEVICE;
	}
	else {
		index = pfq_ifindex(q, dev);
		if (index == -1) {
			return Q_ERROR(q, "PFQ: egress_bind: device not found");
		}
	}

	b.gid = 0;
	b.ifindex = index;
	b.qindex = queue;

        if (setsockopt(q->fd, PF_Q, Q_SO_EGRESS_BIND, &b, sizeof(b)) == -1)
		return Q_ERROR(q, "PFQ: egress bind error");

	return Q_OK(q);
}

int
pfq_egress_unbind(pfq_t *q)
{
        if (setsockopt(q->fd, PF_Q, Q_SO_EGRESS_UNBIND, 0, 0) == -1)
		return Q_ERROR(q, "PFQ: egress unbind error");

	return Q_OK(q);
}


int
pfq_unbind_group(pfq_t *q, int gid, const char *dev, int queue)
{
	struct pfq_so_binding b;

	int index;
	if (strcmp(dev, "any")==0) {
		index = Q_ANY_DEVICE;
	}
	else {
		index = pfq_ifindex(q, dev);
		if (index == -1) {
			return Q_ERROR(q, "PFQ: unbind_group: device not found");
		}
	}

	b.gid = gid;
	b.ifindex = index;
	b.qindex = queue;

	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_UNBIND, &b, sizeof(b)) == -1) {
		return Q_ERROR(q, "PFQ: unbind error");
	}
	return Q_OK(q);
}


int
pfq_unbind(pfq_t *q, const char *dev, int queue)
{
	int gid = q->gid;
	if (gid < 0) {
		return Q_ERROR(q, "PFQ: default group undefined");
	}
	return pfq_unbind_group(q, gid, dev, queue);
}


int
pfq_groups_mask(pfq_t const *q, unsigned long *_mask)
{
	unsigned long mask; socklen_t size = sizeof(mask);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUPS, &mask, &size) == -1) {
		return Q_ERROR(q, "PFQ: get groups error");
	}
	*_mask = mask;
	return Q_OK(q);
}


int
pfq_set_group_computation(pfq_t *q, int gid, struct pfq_lang_computation_descr const *comp)
{
        struct pfq_so_group_computation p = { gid, comp };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_FUNCTION, &p, sizeof(p)) == -1) {
		return Q_ERROR(q, "PFQ: group computation error");
        }

	return Q_OK(q);
}


#define PFQ_ALLOCA(type, value) ({		\
	void * new = alloca(sizeof(type));      \
	*(type *)new = value;			\
	new;					\
})

#define PFQ_ALLOCA_N(type, n, get_value_idx) ({ \
	type * new = alloca(sizeof(type) * (size_t)n);  \
	int Idx;                                \
	for(Idx = 0; Idx < n; Idx++)		\
		new[Idx] = get_value_idx;	\
        (void *)new;				\
})


struct CIDR
{
	uint32_t addr;
	int prefix;
};


enum pfl_type
{
        PFLT_INT
    ,   PFLT_INT8
    ,   PFLT_INT16
    ,   PFLT_INT32
    ,   PFLT_INT64
    ,   PFLT_WORD
    ,   PFLT_WORD8
    ,   PFLT_WORD16
    ,   PFLT_WORD32
    ,   PFLT_WORD64
    ,   PFLT_FUN
    ,   PFLT_STRING
    ,   PFLT_IPv4
    ,   PFLT_CIDR
    ,   PFLT_INTs
    ,   PFLT_INT8s
    ,   PFLT_INT16s
    ,   PFLT_INT32s
    ,   PFLT_INT64s
    ,   PFLT_WORDs
    ,   PFLT_WORD8s
    ,   PFLT_WORD16s
    ,   PFLT_WORD32s
    ,   PFLT_WORD64s
    ,   PFLT_STRINGs
    ,   PFLT_IPv4s
    ,   PFLT_CIDRs
    ,   PFLT_ERROR
};


static
enum pfl_type pfq_get_pfl_type(const char *type)
{
        if (strcasecmp(type, "int"   ) == 0) return PFLT_INT;
        if (strcasecmp(type, "int8"  ) == 0) return PFLT_INT8;
        if (strcasecmp(type, "int16" ) == 0) return PFLT_INT16;
        if (strcasecmp(type, "int32" ) == 0) return PFLT_INT32;
        if (strcasecmp(type, "int64" ) == 0) return PFLT_INT64;
        if (strcasecmp(type, "word"  ) == 0) return PFLT_WORD;
        if (strcasecmp(type, "word8" ) == 0) return PFLT_WORD8;
        if (strcasecmp(type, "word16") == 0) return PFLT_WORD16;
        if (strcasecmp(type, "word32") == 0) return PFLT_WORD32;
        if (strcasecmp(type, "word64") == 0) return PFLT_WORD64;
        if (strcasecmp(type, "fun"   ) == 0) return PFLT_FUN;
        if (strcasecmp(type, "string") == 0) return PFLT_STRING;
        if (strcasecmp(type, "ipv4"  ) == 0) return PFLT_IPv4;
        if (strcasecmp(type, "cidr"  ) == 0) return PFLT_CIDR;

        if (strcasecmp(type, "[int]"   ) == 0) return PFLT_INTs;
        if (strcasecmp(type, "[int8]"  ) == 0) return PFLT_INT8s;
        if (strcasecmp(type, "[int16]" ) == 0) return PFLT_INT16s;
        if (strcasecmp(type, "[int32]" ) == 0) return PFLT_INT32s;
        if (strcasecmp(type, "[int64]" ) == 0) return PFLT_INT64s;
        if (strcasecmp(type, "[word]"  ) == 0) return PFLT_WORDs;
        if (strcasecmp(type, "[word8]" ) == 0) return PFLT_WORD8s;
        if (strcasecmp(type, "[word16]") == 0) return PFLT_WORD16s;
        if (strcasecmp(type, "[word32]") == 0) return PFLT_WORD32s;
        if (strcasecmp(type, "[word64]") == 0) return PFLT_WORD64s;
        if (strcasecmp(type, "[string]") == 0) return PFLT_STRINGs;
        if (strcasecmp(type, "[ipv4]"  ) == 0) return PFLT_IPv4s;
        if (strcasecmp(type, "[cidr]"  ) == 0) return PFLT_CIDRs;

        return PFLT_ERROR;
}


int
pfq_set_group_computation_from_json(pfq_t *q, int gid, const char *input)
{
	struct pfq_lang_computation_descr *prog;
        int n, nfuns;

        cJSON * root = cJSON_Parse(input);
        if (!root) {
                cJSON_Delete(root);
	        return Q_ERROR(q, "PFQ: computation: JSON parse error (cJSON_Parse)");
        }

	nfuns = cJSON_GetArraySize(root);

	prog = alloca(sizeof(size_t) * 2 + sizeof(struct pfq_lang_functional_descr) * (size_t)nfuns);

        memset(prog, 0, sizeof(size_t) * 2 + sizeof(struct pfq_lang_functional_descr) * (size_t)nfuns);

	prog->entry_point = 0;
	prog->size = (size_t)nfuns;

        for (n = 0; n < nfuns; ++n)
        {
                cJSON *fun, *symbol, *link, *args;
                int i, nargs = 0;

                fun = cJSON_GetArrayItem(root, n);

                symbol = cJSON_GetObjectItemCaseSensitive(fun, "funSymbol");
                if (!cJSON_IsString(symbol))
                {
                        cJSON_Delete(root);
	                return Q_ERROR(q, "PFQ: computation: JSON parse error (cJSON_GetObject: funSymbol)!");
                }

                link = cJSON_GetObjectItemCaseSensitive(fun, "funLink");
                if (!cJSON_IsNumber(link))
                {
                        cJSON_Delete(root);
	                return Q_ERROR(q, "PFQ: computation: JSON parse error (cJSON_GetObject: funLink)!");
                }

                args = cJSON_GetObjectItemCaseSensitive(fun, "funArgs");
                if (!cJSON_IsArray(args))
                {
                        cJSON_Delete(root);
	                return Q_ERROR(q, "PFQ: computation: JSON parse error (cJSON_GetObject: funArgs)!");
                }

                for(i = 0; i < cJSON_GetArraySize(args); ++i)
                {
                        cJSON * arg = cJSON_GetArrayItem(args, i);
                        if (!cJSON_IsNull(arg))
                                nargs++;
                }

	        prog->fun[n].symbol = symbol->valuestring;
                prog->fun[n].next = (int)link->valuedouble;

                // parse arguments...
                //

	        for(i = 0; i < (int)min((size_t)nargs, sizeof(prog->fun[n].arg)/sizeof(struct pfq_lang_functional_arg_descr)); ++i)
                {
                        cJSON * arg = cJSON_GetArrayItem(args, i);
                        cJSON * type;

                        if (!cJSON_IsObject(arg))
                        {
                                cJSON_Delete(root);
	                        return Q_ERROR(q, "PFQ: computation: JSON parse error (cJSON_GetObject: arg)!");
                        }

                        type = cJSON_GetObjectItemCaseSensitive(arg, "argType");
                        if (!cJSON_IsString(type))
                        {
                                cJSON_Delete(root);
	                        return Q_ERROR(q, "PFQ: computation: JSON parse error (cJSON_GetObject: argType)!");
                        }

                        switch(pfq_get_pfl_type(type->valuestring))
                        {
                        case PFLT_INT:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Int: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int, (int)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_INT8:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Int8: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int8_t, (int8_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int8_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_INT16:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Int16: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int16_t, (int16_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int16_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_INT32:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Int32: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int32_t, (int32_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int32_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_INT64:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Int64: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int64_t, (int64_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int64_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_WORD8:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Word8: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint8_t, (uint8_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint8_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_WORD16:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Word16: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint16_t, (uint16_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint16_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_WORD32:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Word32: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint32_t, (uint32_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint32_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_WORD64:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Word64: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint64_t, (uint64_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint64_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_FUN:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (Fun: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = NULL;
	                        prog->fun[n].arg[i].size  = (size_t)val->valuedouble;
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_STRING:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsString(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (String: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = val->valuestring;
	                        prog->fun[n].arg[i].size  = 0;
	                        prog->fun[n].arg[i].nelem = -1;

                        } break;
                        case PFLT_IPv4:
                        {
                                cJSON * obj = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                if (!cJSON_IsObject(obj))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (IPv4: argValue)!");
                                }

                                cJSON * val = cJSON_GetObjectItemCaseSensitive(obj, "getHostAddress");
                                if (!cJSON_IsNumber(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (IPv4: argValue)!");
                                }

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint32_t, (uint32_t)val->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint32_t);
	                        prog->fun[n].arg[i].nelem = -1;

	                } break;
                        case PFLT_CIDR:
                        {
                                cJSON * obj = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                cJSON *pair, * addr, *prefix;

                                if (!cJSON_IsObject(obj))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (CIDR: argValue)!");
                                }

                                pair = cJSON_GetObjectItemCaseSensitive(obj, "getNetworkPair");
                                if (!cJSON_IsArray(pair))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (CIDR: array missing)!");
                                }

                                if (cJSON_GetArraySize(pair) != 2)
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error (CIDR: broken array)!");
                                }

                                addr   = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pair, 0), "getHostAddress");
                                prefix = cJSON_GetArrayItem(pair, 1);

	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA(struct CIDR, ((struct CIDR){.addr = (uint32_t)addr->valuedouble,
	                                                                                           .prefix = (int)prefix->valuedouble }));
                                prog->fun[n].arg[i].size  = sizeof(struct CIDR);
	                        prog->fun[n].arg[i].nelem = -1;

                        } break;
                        case PFLT_INTs:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Int]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(int, len, (int)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_INT8s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Int8]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(int8_t, len, (int8_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int8_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_INT16s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Int16]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(int16_t, len, (int16_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int16_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_INT32s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Int32]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(int32_t, len, (int32_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int32_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_INT64s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Int64]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(int64_t, len, (int64_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(int64_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_WORD8s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Word8]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(uint8_t, len, (uint8_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint8_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_WORD16s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Word16]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(uint16_t, len, (uint16_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint16_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_WORD32s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Word32]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(uint32_t, len, (uint32_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint32_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_WORD64s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Word64]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(uint64_t, len, (uint64_t)cJSON_GetArrayItem(val,Idx)->valuedouble);
	                        prog->fun[n].arg[i].size  = sizeof(uint64_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_STRINGs:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([Word64]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
	                        prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(char *, len, (char *)cJSON_GetArrayItem(val,Idx)->valuestring);
	                        prog->fun[n].arg[i].size  = 0;
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_IPv4s:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([IPv4]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);
                                prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(uint32_t, len, (uint32_t)({
                                                                cJSON * obj = cJSON_GetArrayItem(val, Idx);
                                                                cJSON * ip = NULL;
                                                                if (cJSON_IsObject(obj)) {
                                                                        ip = cJSON_GetObjectItemCaseSensitive(obj, "getHostAddress");
                                                                }

                                                                (ip && cJSON_IsNumber(ip)) ? (uint32_t)ip->valuedouble : 0;
                                                                }));
                                prog->fun[n].arg[i].size  = sizeof(uint32_t);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_CIDRs:
                        {
                                cJSON * val = cJSON_GetObjectItemCaseSensitive(arg, "argValue");
                                int len;

                                if (!cJSON_IsArray(val))
                                {
                                        cJSON_Delete(root);
	                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([CIDR]: argValue)!");
                                }

                                len = (int)cJSON_GetArraySize(val);

                                prog->fun[n].arg[i].addr  = PFQ_ALLOCA_N(struct CIDR, len, (struct CIDR)({

                                        cJSON * obj = cJSON_GetArrayItem(val, Idx);
                                        cJSON *pair, * addr, *prefix;

                                        if (!cJSON_IsObject(obj))
                                        {
                                                cJSON_Delete(root);
                                                return Q_ERROR(q, "PFQ: computation: JSON parse error ([CIDR]: argValue)!");
                                        }

                                        pair = cJSON_GetObjectItemCaseSensitive(obj, "getNetworkPair");
                                        if (!cJSON_IsArray(pair))
                                        {
                                                cJSON_Delete(root);
                                                return Q_ERROR(q, "PFQ: computation: JSON parse error (CIDR: array missing)!");
                                        }

                                        if (cJSON_GetArraySize(pair) != 2)
                                        {
                                                cJSON_Delete(root);
                                                return Q_ERROR(q, "PFQ: computation: JSON parse error (CIDR: broken array)!");
                                        }

                                        addr   = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pair, 0), "getHostAddress");
                                        prefix = cJSON_GetArrayItem(pair, 1);

	                                (struct CIDR){.addr = (uint32_t)addr->valuedouble, .prefix = (int)prefix->valuedouble };
                                }));

                                prog->fun[n].arg[i].size  = sizeof(struct CIDR);
	                        prog->fun[n].arg[i].nelem = len;

                        } break;
                        case PFLT_ERROR:
                        default: {
                                static __thread char *e = NULL; free(e);
                                asprintf(&e, "PFQ: computation: JSON argType '%s' not supported!", type->valuestring);
                                cJSON_Delete(root);
	                        return Q_ERROR(q, e);
                        }
                        }
                }
        }

	int ret = pfq_set_group_computation(q, gid, prog);
        cJSON_Delete(root);
        return ret;
}


int
pfq_set_group_computation_from_string(pfq_t *q, int gid, const char *comp)
{
	char filepath[1024] = "/tmp/pfq-lang.XXXXXX";
	int fd;

	fd = mkstemp(filepath);
	if (fd == -1) {
		return Q_ERROR(q, "PFQ: set_group_computation_from_string: mkstemp");
	}

	if (write(fd, comp, strlen(comp)) < 0)
		return Q_ERROR(q, "PFQ: set_group_computation_from_string: write");

	close(fd);

	return pfq_set_group_computation_from_file(q, gid, filepath);
}


int
pfq_set_group_computation_from_file(pfq_t *q, int gid, const char *filepath)
{
	char buffer[1024],
	     *comout;
	size_t chread,
    	       omalloc = 1024,
    	       olen = 0;
	FILE *fp;
	int ret;
        char *stack_yaml;

        if ((stack_yaml = getenv("PFQ_LANG_STACK_YAML"))) {
	        if (snprintf(buffer, 1024, "stack exec --stack-yaml %s -- ~/.local/bin/pfq-lang --json %s", stack_yaml, filepath) < 0) {
		        return Q_ERROR(q, "PFQ: set_group_computation_from_file: snprintf");
	        }
        }
        else {
                if (snprintf(buffer, 1024, "~/.cabal/bin/pfq-lang --json %s", filepath) < 0) {
                        return Q_ERROR(q, "PFQ: set_group_computation_from_file: snprintf");
                }
        }

	fp = popen(buffer, "r");
	if (fp == NULL) {
		return Q_ERROR(q, "PFQ: set_group_computation_from_file: popen");
	}

    	comout = malloc(omalloc);
        if (!comout) {
		return Q_ERROR(q, "PFQ: set_group_computation_from_file: malloc");
	}

	while ((chread = fread(buffer, 1, sizeof(buffer), fp)) != 0) {
	    if (olen + chread >= omalloc) {
	        omalloc *= 2;
	        comout = realloc(comout, omalloc);
	    }
	    memmove(comout + olen, buffer, chread);
	    olen += chread;
	}

	comout[olen] = '\0';

	ret = pfq_set_group_computation_from_json(q, gid, comout);

	free(comout);
	pclose(fp);
        return ret;
}


int
pfq_group_fprog(pfq_t *q, int gid, struct sock_fprog const *f)
{
	struct pfq_so_fprog fprog;

	fprog.gid = gid;
	if (f != NULL)
	{
		fprog.fcode.len = f->len;
		fprog.fcode.filter = f->filter;
	}
	else
	{
		fprog.fcode.len = 0;
		fprog.fcode.filter = NULL;
	}

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_FPROG, &fprog, sizeof(fprog)) == -1) {
		return Q_ERROR(q, "PFQ: set group fprog error");
	}

	return Q_OK(q);
}


int
pfq_group_fprog_reset(pfq_t *q, int gid)
{
	struct sock_fprog null = { 0, NULL };
	if (pfq_group_fprog(q, gid, &null) < 0)
		return Q_ERROR(q, "PFQ: reset group fprog error");
	return Q_OK(q);
}


int
pfq_join_group(pfq_t *q, int gid, unsigned long class_mask, int group_policy)
{
	struct pfq_so_group_join group = { gid, group_policy, class_mask };

	socklen_t size = sizeof(group);
	if (getsockopt(q->fd, PF_Q, Q_SO_GROUP_JOIN, &group, &size) == -1) {
	        return Q_ERROR(q, "PFQ: join group error");
	}

        if (q->gid == -1)
                q->gid = group.gid;

	return Q_VALUE(q, group.gid);
}


int
pfq_leave_group(pfq_t *q, int gid)
{
	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_LEAVE, &gid, sizeof(gid)) == -1) {
	        return Q_ERROR(q, "PFQ: leave group error");
	}
	if (q->gid == gid)
	        q->gid = -1;

	return Q_OK(q);
}


int
pfq_poll(pfq_t *q, long int microseconds /* = -1 -> infinite */)
{
	struct timespec timeout;
	struct pollfd fd = {q->fd, POLLIN, 0 };
        int ret;

	if (q->fd == -1) {
		return Q_ERROR(q, "PFQ: socket not open");
	}

	if (microseconds >= 0) {
		timeout.tv_sec  = microseconds/1000000;
		timeout.tv_nsec = (microseconds%1000000) * 1000;
	}

	ret = ppoll(&fd, 1, microseconds < 0 ? NULL : &timeout, NULL);
	if (ret < 0 && errno != EINTR) {
	    return Q_ERROR(q, "PFQ: ppoll error");
	}
	return Q_OK(q);
}


int
pfq_get_stats(pfq_t const *q, struct pfq_stats *stats)
{
	socklen_t size = sizeof(struct pfq_stats);
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_STATS, stats, &size) == -1) {
		return Q_ERROR(q, "PFQ: get stats error");
	}
	return Q_OK(q);
}


int
pfq_get_group_stats(pfq_t const *q, int gid, struct pfq_stats *stats)
{
	socklen_t size = sizeof(struct pfq_stats);
	stats->recv = (unsigned int)gid;
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUP_STATS, stats, &size) == -1) {
		return Q_ERROR(q, "PFQ: get group stats error");
	}
	return Q_OK(q);
}


int
pfq_get_group_counters(pfq_t const *q, int gid, struct pfq_counters *cs)
{
	socklen_t size = sizeof(struct pfq_counters);
	cs->counter[0] = (unsigned int)gid;

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUP_COUNTERS, cs, &size) == -1) {
		return Q_ERROR(q, "PFQ: get group counters error");
	}
	return Q_OK(q);
}


int
pfq_vlan_filters_enable(pfq_t *q, int gid, int toggle)
{
        struct pfq_so_vlan_toggle value = { gid, 0, toggle };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_VLAN_FILT_TOGGLE, &value, sizeof(value)) == -1) {
	        return Q_ERROR(q, "PFQ: vlan filters");
        }

        return Q_OK(q);
}

int
pfq_vlan_set_filter(pfq_t *q, int gid, int vid)
{
        struct pfq_so_vlan_toggle value = { gid, vid, 1 };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_VLAN_FILT, &value, sizeof(value)) == -1) {
	        return Q_ERROR(q, "PFQ: vlan set filter");
        }

        return Q_OK(q);
}

int pfq_vlan_reset_filter(pfq_t *q, int gid, int vid)
{
        struct pfq_so_vlan_toggle value = { gid, vid, 0 };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_VLAN_FILT, &value, sizeof(value)) == -1) {
	        return Q_ERROR(q, "PFQ: vlan reset filter");
        }

        return Q_OK(q);
}


int
pfq_read(pfq_t *q, struct pfq_socket_queue *nq, long int microseconds)
{
	struct pfq_shared_queue * qd = (struct pfq_shared_queue *)(q->shm_addr);
	unsigned long int data, qver;

        if (unlikely(qd == NULL)) {
		return Q_ERROR(q, "PFQ: read: socket not enabled");
	}

	data = __atomic_load_n(&qd->rx.shinfo, __ATOMIC_RELAXED);

	if (unlikely(PFQ_SHARED_QUEUE_LEN(data) == 0)) {
#ifdef PFQ_USE_POLL
		if (pfq_poll(q, microseconds) < 0)
			return Q_ERROR(q, "PFQ: poll error");
#else
		(void)microseconds;
		nq->len = 0;
		return Q_VALUE(q, (int)0);
#endif
	}

        /* at wrap-around reset Rx slots... */

	qver = PFQ_SHARED_QUEUE_VER(data);

        if (unlikely(((qver+1) & (PFQ_SHARED_QUEUE_VER_MASK^1))== 0))
        {
            char * raw = (char *)(q->rx_queue_addr) + ((qver+1) & 1) * q->rx_queue_size;
            char * end = raw + q->rx_queue_size;
            const pfq_qver_t rst = qver & 1;
            for(; raw < end; raw += q->rx_slot_size)
                ((struct pfq_pkthdr *)raw)->info.commit = rst;
        }

	/* swap the queue... */

        data = __atomic_exchange_n(&qd->rx.shinfo, ((qver+1) << (PFQ_SHARED_QUEUE_LEN_SIZE<<3)), __ATOMIC_RELAXED);

	size_t queue_len = min(PFQ_SHARED_QUEUE_LEN(data), q->rx_slots);

	nq->queue = (char *)(q->rx_queue_addr) + (qver & 1) * q->rx_queue_size;
	nq->index = (unsigned int)qver;
	nq->len   = queue_len;
        nq->slot_size = q->rx_slot_size;

	return Q_VALUE(q, (int)queue_len);
}


int
pfq_recv(pfq_t *q, void *buf, size_t buflen, struct pfq_socket_queue *nq, long int microseconds)
{
	if (buflen < (q->rx_slots * q->rx_slot_size)) {
		return Q_ERROR(q, "PFQ: buffer too small");
	}

	if (pfq_read(q, nq, microseconds) < 0)
		return -1;

	memcpy(buf, nq->queue, q->rx_slot_size * nq->len);
	return Q_OK(q);
}


int
pfq_dispatch(pfq_t *q, pfq_handler_t cb, long int microseconds, char *user)
{
	pfq_iterator_t it, it_end;
	int n = 0;

	if (pfq_read(q, &q->nq, microseconds) < 0)
		return -1;

	it = pfq_socket_queue_begin(&q->nq);
	it_end = pfq_socket_queue_end(&q->nq);

	for(; it != it_end; it = pfq_socket_queue_next(&q->nq, it))
	{
		while (!pfq_pkt_ready(&q->nq, it))
			pfq_relax();

		cb(user, pfq_pkt_header(it), pfq_pkt_data(it));
		n++;
	}
        return Q_VALUE(q, n);
}


int
pfq_bind_tx(pfq_t *q, const char *dev, int queue, int tid)
{
	struct pfq_so_binding b;
        int ifindex;

        ifindex = pfq_ifindex(q, dev);
        if (ifindex == -1)
		return Q_ERROR(q, "PFQ: device not found");

	b = (struct pfq_so_binding){ {tid}, ifindex, queue };

        if (setsockopt(q->fd, PF_Q, Q_SO_TX_BIND, &b, sizeof(b)) == -1)
		return Q_ERROR(q, "PFQ: Tx bind error");

	if (tid != Q_NO_KTHREAD)
		q->tx_num_async++;

	return Q_OK(q);
}


int
pfq_unbind_tx(pfq_t *q)
{
        if (setsockopt(q->fd, PF_Q, Q_SO_TX_UNBIND, NULL, 0) == -1)
		return Q_ERROR(q, "PFQ: Tx unbind error");

	q->tx_num_async = 0;
	return Q_OK(q);
}


int
pfq_send_raw( pfq_t *q
	    , const void *buf
	    , const size_t len
	    , uint64_t nsec
	    , unsigned int copies
	    , int async)
{
        struct pfq_shared_queue *sh_queue = (struct pfq_shared_queue *)(q->shm_addr);
        struct pfq_shared_tx_queue *tx;
        unsigned int index;
        ptrdiff_t offset, *poff_addr;
        uint16_t caplen;
        char *base_addr;
        int tss;

	if (unlikely(q->shm_addr == NULL))
		return Q_ERROR(q, "PFQ: send: socket not enabled");

	if (async != Q_NO_KTHREAD) {
		if (unlikely(q->tx_num_async == 0))
			return Q_ERROR(q, "PFQ: send: socket not bound to async thread");

		tss = (int)pfq_fold((async == Q_ANY_KTHREAD ? pfq_symmetric_hash(buf) : (unsigned int)async)
				   ,(unsigned int)q->tx_num_async);

		tx = (struct pfq_shared_tx_queue *)&sh_queue->tx_async[tss];
	}
	else {
		tss = -1;
		tx = (struct pfq_shared_tx_queue *)&sh_queue->tx;
	}

	index = __atomic_load_n(&tx->cons.index, __ATOMIC_RELAXED);
	if (index == __atomic_load_n(&tx->prod.index, __ATOMIC_RELAXED)) {
		++index;
		poff_addr = (index & 1) ? &tx->prod.off1 : &tx->prod.off0;
                __atomic_store_n(poff_addr, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&tx->prod.index, index, __ATOMIC_RELEASE);
	}
	else {
		poff_addr = (index & 1) ? &tx->prod.off1 : &tx->prod.off0;
	}

	base_addr = q->tx_queue_addr + q->tx_queue_size * (size_t)(2 * (1+tss) + (index & 1 ? 1 : 0));
        offset = __atomic_load_n(poff_addr, __ATOMIC_RELAXED);
	caplen = (uint16_t)min(len, q->tx_slot_size - sizeof(struct pfq_pkthdr));

        /* ensure there's enough space for the current packet */

	if (likely(((size_t)(offset) + q->tx_slot_size) < q->tx_queue_size)) {
		struct pfq_pkthdr *hdr = (struct pfq_pkthdr *)(base_addr + offset);
		hdr->tstamp.tv64       = nsec;
		hdr->len	       = (uint16_t)len;
		hdr->caplen	       = (uint16_t)caplen;
		hdr->info.data.copies  = copies;
		__builtin_memcpy(hdr+1, buf, caplen);
                __atomic_store_n(poff_addr, offset + (ptrdiff_t)q->tx_slot_size, __ATOMIC_RELEASE);
		return Q_VALUE(q, (int)len);
	}

	return Q_VALUE(q, 0);
}


int
pfq_send( pfq_t *q
	, const void *ptr
	, size_t len
	, unsigned int copies
	, size_t sync)
{
	int ret;
	retry:
	ret = pfq_send_raw(q, ptr, len, 0, copies, Q_NO_KTHREAD);
	if (ret == 0 || ++q->tx_attempt == sync) {
		q->tx_attempt = 0;
		pfq_sync_queue(q, 0);
		if (ret == 0)
			goto retry;
	}
	return ret;
}


int
pfq_sync_queue(pfq_t *q, int queue)
{
#if 1
	if (setsockopt(q->fd, PF_Q, Q_SO_TX_QUEUE_XMIT, &queue, sizeof(queue)) == -1)
#else
        if (ioctl(q->fd, QIOCTX, queue) == -1)
#endif
		return Q_ERROR(q, "PFQ: Tx queue");
        return Q_OK(q);
}


size_t
pfq_mem_size(pfq_t const *q)
{
	return q->shm_size;
}


const void *
pfq_mem_addr(pfq_t const *q)
{
	return q->shm_addr;
}


int
pfq_id(pfq_t *q)
{
	return q->id;
}


int
pfq_group_id(pfq_t *q)
{
	return q->gid;
}


int pfq_get_fd(pfq_t const *q)
{
	return q->fd;
}
