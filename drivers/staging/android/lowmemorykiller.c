/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/notifier.h>
#include <linux/delay.h>
// ACOS_MOD_BEGIN {fwk_crash_log_collection}
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
// ACOS_MOD_END {fwk_crash_log_collection}

#if defined (CONFIG_MTK_AEE_FEATURE) && defined (CONFIG_MT_ENG_BUILD)
#include <linux/aee.h>
#include <linux/disp_assert_layer.h>
uint32_t in_lowmem = 0;
#endif

#ifdef CONFIG_HIGHMEM
#include <linux/highmem.h>
#endif

#ifdef CONFIG_ION_MTK
#include <linux/ion_drv.h>
#endif

/* From page_alloc.c, for urgent allocations in preemptible situation */
extern void show_free_areas_minimum(void);

#ifdef CONFIG_ZRAM
extern void mlog(int type);
#endif

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
#define CONVERT_ADJ(x) ((x * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE)
#define REVERT_ADJ(x)  (x * (-OOM_DISABLE + 1) / OOM_SCORE_ADJ_MAX)
#else
#define CONVERT_ADJ(x) (x)
#define REVERT_ADJ(x) (x)
#endif // CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES

static short lowmem_debug_adj = CONVERT_ADJ(1);
#ifdef CONFIG_MT_ENG_BUILD
static short lowmem_kernel_warn_adj = CONVERT_ADJ(0);
#define output_expect(x) likely(x)
static uint32_t enable_candidate_log = 1;
#else
#define output_expect(x) unlikely(x)
static uint32_t enable_candidate_log = 0;
#endif
static DEFINE_MUTEX(lowmem_shrink_mutex);
static uint32_t lowmem_debug_level = 1;

#ifdef CONFIG_MTK_LCA_RAM_OPTIMIZE
extern phys_addr_t get_max_DRAM_size(void);
#define MAX_SIZE_OF_L0 256*1024*1024

static short lowmem_adj[9] = {0};
int lowmem_minfree[9] = {0};
static int lowmem_adj_size = 0;
static int lowmem_minfree_size = 0;

static __initdata short lowmem_adj_L0[9] = {
	CONVERT_ADJ(0),
	CONVERT_ADJ(1),
	CONVERT_ADJ(2),
	CONVERT_ADJ(4),
	CONVERT_ADJ(6),
	CONVERT_ADJ(8),
	CONVERT_ADJ(9),
	CONVERT_ADJ(12),
	CONVERT_ADJ(15),
};

static __initdata short lowmem_adj_L1[6] = {
	CONVERT_ADJ(0),
	CONVERT_ADJ(1),
	CONVERT_ADJ(2),
	CONVERT_ADJ(3),
	CONVERT_ADJ(9),
	CONVERT_ADJ(17),
};

static __initdata int lowmem_minfree_L0[9] = {
	4 * 256,	/*  0 ->  4MB */
	12 * 256,	/*  1 -> 12MB */
	20 * 256,	/*  2 -> 20MB */
	24 * 256,	/*  4 -> 24MB */
	28 * 256,	/*  6 -> 28MB */
	32 * 256,	/*  8 -> 32MB */
	36 * 256,	/*  9 -> 36MB */
	40 * 256,	/* 12 -> 40MB */
	48 * 256,	/* 15 -> 48MB */
};

// Follow KK 512 project setting
static __initdata int lowmem_minfree_L1[6] = { 
	24 * 256,	/*  0 -> 24MB */
	31 * 256,	/*  1 -> 31MB */
	38 * 256,	/*  2 -> 38MB */
	48 * 256,	/*  3 -> 48MB */
	55 * 256,	/*  9 -> 55MB */
	67 * 256,	/* 18 -> 67MB */
};
#else //CONFIG_MTK_LCA_RAM_OPTIMIZE
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;
#endif //CONFIG_MTK_LCA_RAM_OPTIMIZE

#ifdef CONFIG_HIGHMEM
static int total_low_ratio = 1;
#endif

static unsigned long lowmem_deathpending_timeout;
static unsigned long lowmem_kill_timeout;

// ACOS_MOD_BEGIN {fwk_crash_log_collection}
// Declarations
void mali_session_memory_tracking_lmk();
void ion_mm_heap_memory_detail_lmk();
void show_free_areas_minimum_lmk(void);

// Constants
static int BUFFER_SIZE = 16*1024;
static int ELEMENT_SIZE = 256;

// Variables
static char *lmk_log_buffer;
static char *buffer_end;
static char *head;
static char *kill_msg_index;
static char *previous_crash;
static int buffer_remaining = 0;
static int foreground_kill = 0;

void lmk_add_to_buffer(const char * fmt, ...)
{
	if (lmk_log_buffer) {
		if (head >= buffer_end) {
			// Don't add more logs buffer is full
			return;
		}
		if (buffer_remaining > 0) {
			va_list args;
			va_start(args, fmt);
			int added_size = vsnprintf(head, buffer_remaining, fmt, args);
			va_end(args);
			if (added_size > 0) {
				// Add 1 for null terminator
				added_size = added_size + 1;
				buffer_remaining = buffer_remaining - added_size;
				head = head + added_size;
			}
		}
	}
}

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_warn(x);			\
		if (foreground_kill)			\
			lmk_add_to_buffer(x);		\
	} while (0)

// In lowmem_print macro only added the lines 'if (foreground_kill)' and 'lmk_add_to_buffer(x)'
// ACOS_MOD_END {fwk_crash_log_collection}

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	} while_each_thread(p, t);

	return 0;
}

static int lowmem_shrink(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES) - totalreserve_pages;
	int other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM);

	unsigned long nr_to_scan = sc->nr_to_scan;
	int print_extra_info = 0;
	static unsigned long lowmem_print_extra_info_timeout = 0;

#ifdef CONFIG_MTK_LCA_RAM_OPTIMIZE
	int other_anon = global_page_state(NR_INACTIVE_ANON) - global_page_state(NR_ACTIVE_ANON);
#endif
#ifdef CONFIG_MT_ENG_BUILD	
	/*dump memory info when framework low memory*/
	int pid_dump = -1; // process which need to be dump
	int pid_sec_mem = -1;
	int max_mem = 0;
#endif // CONFIG_MT_ENG_BUILD
	
	/* Avoid to have too many parallel executions from direct reclaim when
       memory pressure is really critical. The cost of going through task
       list to find one to kill is too high when allow parallel execution */
	if (time_before_eq(jiffies, lowmem_kill_timeout) && (!current_is_kswapd())) {
		lowmem_print(5, "skip kill for direct reclaim within kill timeout\n");
		return 0;
	}

	/* We are in MTKPASR stage! */
	if (unlikely(current->flags & PF_MTKPASR)) {
		return -1;
	}

	if (nr_to_scan > 0) {
		if (mutex_lock_interruptible(&lowmem_shrink_mutex) < 0) {
			lowmem_print(4, "lowmem_shrink lock failed\n");
			return 0;
		}
	}

#ifdef CONFIG_ZRAM
	other_file -= total_swapcache_pages();
#endif

#ifdef CONFIG_HIGHMEM
    	/* 
	 * Check whether it is caused by low memory in normal zone!
	 * This will help solve over-reclaiming situation while total free pages is enough, but normal zone is under low memory.
	 */
	if (gfp_zone(sc->gfp_mask) == ZONE_NORMAL) {
		int nid;
		struct zone *z;

		/* Restore other_free */
		other_free += totalreserve_pages;

		/* Go through all memory nodes & substract (free, file) from ZONE_HIGHMEM */
		for_each_online_node(nid) {
			z = &NODE_DATA(nid)->node_zones[ZONE_HIGHMEM];
			other_free -= zone_page_state(z, NR_FREE_PAGES);
			other_file -= zone_page_state(z, NR_FILE_PAGES);
			/* Don't substract NR_SHMEM twice! */
			other_file += zone_page_state(z, NR_SHMEM);
			/* Subtract high watermark of normal zone */
			z = &NODE_DATA(nid)->node_zones[ZONE_NORMAL];
			other_free -= high_wmark_pages(z);
		}

		/* Normalize */
		other_free *= total_low_ratio;
		other_file *= total_low_ratio;
	}
#endif
	/* Let it be positive or zero */
	if (other_free < 0) {
		/* lowmem_print(1, "Original other_free [%d] is too low!\n", other_free); */
		other_free = 0;
	}

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		minfree = lowmem_minfree[i];
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}
#ifdef CONFIG_MTK_LCA_RAM_OPTIMIZE
	// For GB3 CR ALPS00602722: walkaround CTS issue
	if (min_score_adj < 9 && other_anon > 70 * 256) {
		// if other_anon > 70MB, don't kill adj <= 8
		min_score_adj = 9;
	}
#endif

	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %lu, %x, ofree %d %d, ma %hd\n",
				nr_to_scan, sc->gfp_mask, other_free,
				other_file, min_score_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0 || min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %lu, %x, return %d\n",
			     nr_to_scan, sc->gfp_mask, rem);
    /*
     * disable indication if low memory
     */
#if defined (CONFIG_MTK_AEE_FEATURE) && defined (CONFIG_MT_ENG_BUILD)
		if (in_lowmem) {
			in_lowmem = 0;
			//DAL_LowMemoryOff();
			lowmem_print(1, "LowMemoryOff\n");
		}
#endif
		if (nr_to_scan > 0)
			mutex_unlock(&lowmem_shrink_mutex);
		return rem;
	}
	selected_oom_score_adj = min_score_adj;

	// add debug log
	if (output_expect(enable_candidate_log)) {
		if (min_score_adj <= lowmem_debug_adj) {
			if (time_after_eq(jiffies, lowmem_print_extra_info_timeout)) {
				lowmem_print_extra_info_timeout = jiffies + HZ;
				print_extra_info = 1;
			}
			else if (min_score_adj <= 0) {
				print_extra_info = 1;
            }
		}
		if (print_extra_info) {
			lowmem_print(1, "======low memory killer=====\n");
			lowmem_print(1, "Free memory other_free: %d, other_file:%d pages\n", other_free, other_file);
		}		
	}	

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		if (time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			if (test_task_flag(tsk, TIF_MEMDIE)) {
#ifdef CONFIG_MT_ENG_BUILD
				static pid_t last_dying_pid;
				if (last_dying_pid != tsk->pid) {
					lowmem_print(1, "lowmem_shrink return directly, due to  %d (%s) is dying\n",
						tsk->pid, tsk->comm);
					last_dying_pid = tsk->pid;
				}
#endif
				rcu_read_unlock();
				/* give the system time to free up the memory
				 * to avoid too many lowmem_shrinks eating cpu
				 */
				msleep_interruptible(20);
				mutex_unlock(&lowmem_shrink_mutex);
				return 0;
			}
		}

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score_adj = p->signal->oom_score_adj;
		
		if (output_expect(enable_candidate_log)) {
			if (print_extra_info) {
#ifdef CONFIG_ZRAM
				lowmem_print(1, "Candidate %d (%s), adj %d, score_adj %d, rss %lu, rswap %lu, to kill\n",
        				     p->pid, p->comm, REVERT_ADJ(oom_score_adj), oom_score_adj, get_mm_rss(p->mm),
        				     get_mm_counter(p->mm, MM_SWAPENTS));
#else // CONFIG_ZRAM
				lowmem_print(1, "Candidate %d (%s), adj %d, score_adj %d, rss %lu, to kill\n",
        				     p->pid, p->comm, REVERT_ADJ(oom_score_adj), oom_score_adj, get_mm_rss(p->mm));
#endif // CONFIG_ZRAM
			}
		}

		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}

		tasksize = get_mm_rss(p->mm);
#ifdef CONFIG_ZRAM
		tasksize += get_mm_counter(p->mm, MM_SWAPENTS);
#endif
		task_unlock(p);
		if (tasksize <= 0)
			continue;

#ifdef CONFIG_MT_ENG_BUILD
		/*
         	 * dump memory info when framework low memory:
         	 * record the first two pid which consumed most memory.
         	 */
		if (tasksize > max_mem) {
			max_mem = tasksize;
			pid_sec_mem = pid_dump;
			pid_dump = p->pid;
		}
#endif
		
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
#ifdef CONFIG_MTK_LCA_RAM_OPTIMIZE
		// For KK CR ALPS01426325: walkaround CTS issue
		// if cached > 30MB, don't kill ub:secureRandom while its adj is 9
		if (!strcmp(p->comm, "ub:secureRandom") && (REVERT_ADJ(oom_score_adj)==9) && (other_file > 30*256)) {
		    lowmem_print(1, "select but ignore '%s' (%d), oom_score_adj %d, oom_adj %d, size %d, to kill\n" \
		                    "cache %ldkB is below limit %ldkB",
			     p->comm, p->pid, oom_score_adj, REVERT_ADJ(oom_score_adj), tasksize,			     
			     other_file * (long)(PAGE_SIZE / 1024),
			     minfree * (long)(PAGE_SIZE / 1024));
		    continue;
		}
#endif		
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_score_adj = oom_score_adj;
		lowmem_print(2, "select '%s' (%d), adj %d, score_adj %hd, size %d, to kill\n",
			     p->comm, p->pid, REVERT_ADJ(oom_score_adj), oom_score_adj, tasksize);
	}

// ACOS_MOD_BEGIN {fwk_crash_log_collection}
#ifndef CONFIG_MT_ENG_BUILD
	if (lmk_log_buffer && selected && selected_oom_score_adj == 0 && !print_extra_info) {
		print_extra_info = 1;
		foreground_kill = 1;
		head = lmk_log_buffer;
		buffer_remaining = BUFFER_SIZE;
		if (kill_msg_index && previous_crash) {
			strncpy(previous_crash, kill_msg_index, ELEMENT_SIZE);
		}
		lowmem_print(1, "======low memory killer=====\n");
		lowmem_print(1, "Free memory other_free: %d, other_file:%d pages\n", other_free, other_file);
		if (gfp_zone(sc->gfp_mask) == ZONE_NORMAL) {
			lowmem_print(1, "ZONE_NORMAL\n");
		} else {
			lowmem_print(1, "ZONE_HIGHMEM\n");
		}

		for_each_process(tsk) {
			struct task_struct *p2;
			short oom_score_adj2;

			if (tsk->flags & PF_KTHREAD)
				continue;

			/* if task no longer has any memory ignore it */
			if (test_task_flag(tsk, TIF_MM_RELEASED))
				continue;

			p2 = find_lock_task_mm(tsk);
			if (!p2)
				continue;

			oom_score_adj2 = p2->signal->oom_score_adj;
#ifdef CONFIG_ZRAM
			lowmem_print(1, "Candidate %d (%s), adj %d, score_adj %d, rss %lu, rswap %lu, to kill\n",
				p2->pid, p2->comm, REVERT_ADJ(oom_score_adj2), oom_score_adj2, get_mm_rss(p2->mm),
				get_mm_counter(p2->mm, MM_SWAPENTS));
#else // CONFIG_ZRAM
			lowmem_print(1, "Candidate %d (%s), adj %d, score_adj %d, rss %lu, to kill\n",
				p2->pid, p2->comm, REVERT_ADJ(oom_score_adj2), oom_score_adj2, get_mm_rss(p2->mm));
#endif // CONFIG_ZRAM
			task_unlock(p2);
		}
		mali_session_memory_tracking_lmk();
		kill_msg_index = head;
	}
#endif //CONFIG_MT_ENG_BUILD
// ACOS_MOD_END {fwk_crash_log_collection}

	if (selected) {
		lowmem_print(1, "Killing '%s' (%d), adj %d, score_adj %hd,\n" \
				"   to free %ldkB on behalf of '%s' (%d) because\n" \
				"   cache %ldkB is below limit %ldkB for oom_score_adj %hd\n" \
				"   Free memory is %ldkB above reserved\n",
			     selected->comm, selected->pid,
				 REVERT_ADJ(selected_oom_score_adj),
			     selected_oom_score_adj,
			     selected_tasksize * (long)(PAGE_SIZE / 1024),
			     current->comm, current->pid,
			     other_file * (long)(PAGE_SIZE / 1024),
			     minfree * (long)(PAGE_SIZE / 1024),
			     min_score_adj,
			     other_free * (long)(PAGE_SIZE / 1024));
		lowmem_deathpending_timeout = jiffies + HZ;
		/* for skipping scan from direct reclaim in next 100ms*/
		lowmem_kill_timeout = jiffies + HZ/10;

// ACOS_MOD_BEGIN {fwk_crash_log_collection}
		if (print_extra_info) {
			show_free_areas_minimum();
			if (foreground_kill) {
				lowmem_print(1, "\nlow memory info:\n");
				show_free_areas_minimum_lmk();
			}
#ifdef CONFIG_ION_MTK
			/* Show ION status */
			lowmem_print(1, "ion_mm_heap_total_memory[%ld]\n",(unsigned long)ion_mm_heap_total_memory());
			ion_mm_heap_memory_detail();
#endif
		}
// ACOS_MOD_END {fwk_crash_log_collection}

		/*
		 * when kill adj=0 process trigger kernel warning, only in MTK internal eng load
		 */
#if defined (CONFIG_MTK_AEE_FEATURE) && defined (CONFIG_MT_ENG_BUILD)
		if (selected_oom_score_adj <= lowmem_kernel_warn_adj) { // can set lowmem_kernel_warn_adj=16 for test
			#define MSG_SIZE_TO_AEE 70
			char msg_to_aee[MSG_SIZE_TO_AEE];
			lowmem_print(1, "low memory trigger kernel warning\n");
                        
			if (pid_dump == selected->pid)
  				pid_dump = pid_sec_mem;
   			snprintf(msg_to_aee, MSG_SIZE_TO_AEE, "please contact AP/AF memory module owner[pid:%d]\n", pid_dump);
 			aee_kernel_warning_api("LMK", 0, DB_OPT_DEFAULT|DB_OPT_DUMPSYS_ACTIVITY|DB_OPT_LOW_MEMORY_KILLER
                        		| DB_OPT_PID_MEMORY_INFO /*for smaps and hprof*/
                        		| DB_OPT_PROCESS_COREDUMP
                        		| DB_OPT_DUMPSYS_SURFACEFLINGER
                        		| DB_OPT_DUMPSYS_PROCSTATS,
                                "Framework low memory\nCRDISPATCH_KEY:FLM_APAF", msg_to_aee);
		}
#endif
		/*
		 * show an indication if low memory
		 */
#if defined (CONFIG_MTK_AEE_FEATURE) && defined (CONFIG_MT_ENG_BUILD)
		if (!in_lowmem && selected_oom_score_adj <= lowmem_debug_adj) {
			in_lowmem = 1;
			//DAL_LowMemoryOn();
			lowmem_print(1, "LowMemoryOn\n");
			//aee_kernel_warning(module_name, lowmem_warning);
		}
#endif

#ifdef CONFIG_ZRAM
		mlog(1);
#endif
		send_sig(SIGKILL, selected, 0);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem -= selected_tasksize;
		rcu_read_unlock();
		msleep_interruptible(20);
	} else
		rcu_read_unlock();

	mutex_unlock(&lowmem_shrink_mutex);
	lowmem_print(4, "lowmem_shrink %lu, %x, return %d\n",
		     nr_to_scan, sc->gfp_mask, rem);

	/* ACOS_MOD_BEGIN {fwk_crash_log_collection} */
	if (foreground_kill) {
		ion_mm_heap_memory_detail_lmk();
	}
	/* ACOS_MOD_END {fwk_crash_log_collection} */

	foreground_kill = 0; ///ACOS_MOD_ONELINE {fwk_crash_log_collection}

	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

// ACOS_MOD_BEGIN {fwk_crash_log_collection}
static int lowmem_proc_show(struct seq_file *m, void *v)
{
	if (!lmk_log_buffer) {
		seq_printf(m, "lmk_logs are not functioning - something went wrong during init");
		return 0;
	}
	char *ptr = lmk_log_buffer;
	while (ptr < head) {
		seq_printf(m, ptr, "\n");
		int cur_line_len = strlen(ptr);
		if (cur_line_len <= 0){
			break;
		}
		// add 1 to skip the null terminator for C Strings
		ptr = ptr + cur_line_len + 1;
	}
	if (previous_crash && previous_crash[0] !='\0'){
		seq_printf(m, "previous crash: \n");
		seq_printf(m, previous_crash, "\n");
	}
	return 0;
}

static int lowmem_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, lowmem_proc_show, NULL);
}

static const struct file_operations lowmem_proc_fops = {
	.open       = lowmem_proc_open,
	.read       = seq_read,
	.release    = single_release
};
// ACOS_MOD_END {fwk_crash_log_collection}

static int __init lowmem_init(void)
{
#ifdef CONFIG_HIGHMEM
	unsigned long normal_pages;
#endif

#ifdef CONFIG_ZRAM
	vm_swappiness = 100;
#endif

#ifdef CONFIG_MTK_LCA_RAM_OPTIMIZE
  if(get_max_DRAM_size() > MAX_SIZE_OF_L0){
    lowmem_adj_size = ARRAY_SIZE(lowmem_adj_L1);
    lowmem_minfree_size = ARRAY_SIZE(lowmem_adj_L1);
    memcpy(lowmem_adj, lowmem_adj_L1, sizeof(lowmem_adj_L1));
    memcpy(lowmem_minfree, lowmem_minfree_L1, sizeof(lowmem_minfree_L1));
  }	
  else{
  	lowmem_adj_size = ARRAY_SIZE(lowmem_adj_L0);
    lowmem_minfree_size = ARRAY_SIZE(lowmem_adj_L0);
    memcpy(lowmem_adj, lowmem_adj_L0, sizeof(lowmem_adj_L0));
    memcpy(lowmem_minfree, lowmem_minfree_L0, sizeof(lowmem_minfree_L0));
  }  	
#endif

	register_shrinker(&lowmem_shrinker);

#ifdef CONFIG_HIGHMEM
	normal_pages = totalram_pages - totalhigh_pages;
	total_low_ratio = (totalram_pages + normal_pages - 1) / normal_pages;
	printk(KERN_ALERT "[LMK]total_low_ratio[%d] - totalram_pages[%lu] - totalhigh_pages[%lu]\n", total_low_ratio, totalram_pages, totalhigh_pages);
#endif

	// ACOS_MOD_BEGIN {fwk_crash_log_collection}
	proc_create("lmk_logs", 0, NULL, &lowmem_proc_fops);
	lmk_log_buffer = kzalloc(BUFFER_SIZE, GFP_KERNEL);
	if (lmk_log_buffer) {
		buffer_end = lmk_log_buffer + BUFFER_SIZE;
		head = lmk_log_buffer;
		buffer_remaining = BUFFER_SIZE;
		kill_msg_index = NULL;
		previous_crash = kzalloc(ELEMENT_SIZE, GFP_KERNEL);
		if (!previous_crash) {
			printk(KERN_ALERT "unable to allocate previous_crash for /proc/lmk_logs - previous_crash will not be logged");
		}
	} else {
		printk(KERN_ALERT "unable to allocate buffer for /proc/lmk_logs - feature will be disabled");
	}
	// ACOS_MOD_END {fwk_crash_log_collection}

	return 0;
}

static void __exit lowmem_exit(void)
{
	// ACOS_MOD_BEGIN {fwk_crash_log_collection}
	kfree(lmk_log_buffer);
	kfree(previous_crash);
	// ACOS_MOD_END {fwk_crash_log_collection}
	unregister_shrinker(&lowmem_shrinker);
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

/*
 * get_min_free_pages
 * returns the low memory killer watermark of the given pid,
 * When the system free memory is lower than the watermark, the LMK (low memory
 * killer) may try to kill processes.
 */
int get_min_free_pages(pid_t pid)
{
    struct task_struct *p = 0;
    int target_oom_adj = 0;
    int i = 0;
    int array_size = ARRAY_SIZE(lowmem_adj);

    if (lowmem_adj_size < array_size)
            array_size = lowmem_adj_size;
    if (lowmem_minfree_size < array_size)
            array_size = lowmem_minfree_size;

    for_each_process(p) {
        /* search pid */
        if (p->pid == pid) {
            task_lock(p);
            target_oom_adj = p->signal->oom_score_adj;
            task_unlock(p);
            /* get min_free value of the pid */
            for (i = array_size - 1; i >= 0; i--) {
                if (target_oom_adj >= lowmem_adj[i]) {
                    lowmem_print(3, KERN_INFO"pid: %d, target_oom_adj = %d, "
                            "lowmem_adj[%d] = %d, lowmem_minfree[%d] = %d\n",
                            pid, target_oom_adj, i, lowmem_adj[i], i,
                            lowmem_minfree[i]);
                    return lowmem_minfree[i];
                }
            }
            goto out; 
        }
    }

out:
    lowmem_print(3, KERN_ALERT"[%s]pid: %d, adj: %d, lowmem_minfree = 0\n", 
            __FUNCTION__, pid, p->signal->oom_score_adj);
    return 0;
}
EXPORT_SYMBOL(get_min_free_pages);

/* Query LMK minfree settings */
/* To query default value, you can input index with value -1. */
size_t query_lmk_minfree(int index)
{
	int which;

	/* Invalid input index, return default value */
	if (index < 0) {
		return lowmem_minfree[2];
	}
	
	/* Find a corresponding output */
	which = 5;
	do {
		if (lowmem_adj[which] <= index) {
			break;
		}
	} while (--which >= 0);

	/* Fix underflow bug */
	which = (which < 0)? 0 : which;

	return lowmem_minfree[which];
}
EXPORT_SYMBOL(query_lmk_minfree);

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
__module_param_call(MODULE_PARAM_PREFIX, adj,
		    &lowmem_adj_array_ops,
		    .arr = &__param_arr_adj,
		    S_IRUGO | S_IWUSR, -1);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static int debug_adj_set(const char *val, const struct kernel_param *kp)
{
    const int ret = param_set_uint(val, kp);
    lowmem_debug_adj = lowmem_oom_adj_to_oom_score_adj(lowmem_debug_adj);
    return ret;
}

static struct kernel_param_ops debug_adj_ops = {
	.set = &debug_adj_set,
	.get = &param_get_uint,
};

module_param_cb(debug_adj, &debug_adj_ops, &lowmem_debug_adj, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(debug_adj, short);
#else
module_param_named(debug_adj, lowmem_debug_adj, short, S_IRUGO | S_IWUSR);
#endif
module_param_named(candidate_log, enable_candidate_log, uint, S_IRUGO | S_IWUSR);

late_initcall(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");
