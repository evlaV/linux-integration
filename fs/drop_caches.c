// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the manual drop-all-pagecache function
 */

#include <linux/pagemap.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/writeback.h>
#include <linux/sysctl.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/atomic.h>
#include "internal.h"

/* Target for limited page cache drop: 2GB */
#define LIMITED_DROP_TARGET_BYTES (2UL * 1024 * 1024 * 1024)
#define LIMITED_DROP_TARGET_PAGES (LIMITED_DROP_TARGET_BYTES >> PAGE_SHIFT)

/* A global variable is a bit ugly, but it keeps the code simple */
int sysctl_drop_caches;

static void drop_pagecache_sb(struct super_block *sb, void *arg)
{
	atomic_long_t *remaining_pages = arg;
	struct inode *inode, *toput_inode = NULL;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		unsigned long invalidated;

		/* Check limit if one is provided */
		if (remaining_pages && atomic_long_read(remaining_pages) <= 0)
			break;

		spin_lock(&inode->i_lock);
		/*
		 * We must skip inodes in unusual state. We may also skip
		 * inodes without pages but we deliberately won't in case
		 * we need to reschedule to avoid softlockups.
		 */
		if ((inode->i_state & (I_FREEING|I_WILL_FREE|I_NEW)) ||
		    (mapping_empty(inode->i_mapping) && !need_resched())) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		spin_unlock(&sb->s_inode_list_lock);

		invalidated = invalidate_mapping_pages(inode->i_mapping, 0, -1);
		if (remaining_pages)
			atomic_long_sub(invalidated, remaining_pages);

		iput(toput_inode);
		toput_inode = inode;

		cond_resched();
		spin_lock(&sb->s_inode_list_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);
	iput(toput_inode);
}

int drop_caches_sysctl_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret)
		return ret;
	if (write) {
		static int stfu;

		if (sysctl_drop_caches & 1) {
			lru_add_drain_all();
			iterate_supers(drop_pagecache_sb, NULL);
			count_vm_event(DROP_PAGECACHE);
		}
		if (sysctl_drop_caches & 2) {
			drop_slab();
			count_vm_event(DROP_SLAB);
		}
		if (sysctl_drop_caches == 4) {
			atomic_long_t remaining_pages = ATOMIC_LONG_INIT(LIMITED_DROP_TARGET_PAGES);
			lru_add_drain_all();
			iterate_supers(drop_pagecache_sb, &remaining_pages);
			count_vm_event(DROP_PAGECACHE_LIMITED);
		}
		if (!stfu) {
			pr_info("%s (%d): drop_caches: %d\n",
				current->comm, task_pid_nr(current),
				sysctl_drop_caches);
		}
		stfu |= sysctl_drop_caches & 3;
	}
	return 0;
}
