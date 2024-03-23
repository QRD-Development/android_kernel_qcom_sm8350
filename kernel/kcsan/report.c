// SPDX-License-Identifier: GPL-2.0

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stacktrace.h>

#include "kcsan.h"
#include "encoding.h"

/*
 * Max. number of stack entries to show in the report.
 */
#define NUM_STACK_ENTRIES 64

/*
 * Other thread info: communicated from other racing thread to thread that set
 * up the watchpoint, which then prints the complete report atomically. Only
 * need one struct, as all threads should to be serialized regardless to print
 * the reports, with reporting being in the slow-path.
 */
static struct {
	const volatile void	*ptr;
	size_t			size;
	int			access_type;
	int			task_pid;
	int			cpu_id;
	unsigned long		stack_entries[NUM_STACK_ENTRIES];
	int			num_stack_entries;
} other_info = { .ptr = NULL };

/*
 * Information about reported data races; used to rate limit reporting.
 */
struct report_time {
	/*
	 * The last time the data race was reported.
	 */
	unsigned long time;

	/*
	 * The frames of the 2 threads; if only 1 thread is known, one frame
	 * will be 0.
	 */
	unsigned long frame1;
	unsigned long frame2;
};

/*
 * Since we also want to be able to debug allocators with KCSAN, to avoid
 * deadlock, report_times cannot be dynamically resized with krealloc in
 * rate_limit_report.
 *
 * Therefore, we use a fixed-size array, which at most will occupy a page. This
 * still adequately rate limits reports, assuming that a) number of unique data
 * races is not excessive, and b) occurrence of unique data races within the
 * same time window is limited.
 */
#define REPORT_TIMES_MAX (PAGE_SIZE / sizeof(struct report_time))
#define REPORT_TIMES_SIZE                                                      \
	(CONFIG_KCSAN_REPORT_ONCE_IN_MS > REPORT_TIMES_MAX ?                   \
		 REPORT_TIMES_MAX :                                            \
		 CONFIG_KCSAN_REPORT_ONCE_IN_MS)
static struct report_time report_times[REPORT_TIMES_SIZE];

/*
 * This spinlock protects reporting and other_info, since other_info is usually
 * required when reporting.
 */
static DEFINE_SPINLOCK(report_lock);

/*
 * Checks if the data race identified by thread frames frame1 and frame2 has
 * been reported since (now - KCSAN_REPORT_ONCE_IN_MS).
 */
static bool rate_limit_report(unsigned long frame1, unsigned long frame2)
{
	struct report_time *use_entry = &report_times[0];
	unsigned long invalid_before;
	int i;

	BUILD_BUG_ON(CONFIG_KCSAN_REPORT_ONCE_IN_MS != 0 && REPORT_TIMES_SIZE == 0);

	if (CONFIG_KCSAN_REPORT_ONCE_IN_MS == 0)
		return false;

	invalid_before = jiffies - msecs_to_jiffies(CONFIG_KCSAN_REPORT_ONCE_IN_MS);

	/* Check if a matching data race report exists. */
	for (i = 0; i < REPORT_TIMES_SIZE; ++i) {
		struct report_time *rt = &report_times[i];

		/*
		 * Must always select an entry for use to store info as we
		 * cannot resize report_times; at the end of the scan, use_entry
		 * will be the oldest entry, which ideally also happened before
		 * KCSAN_REPORT_ONCE_IN_MS ago.
		 */
		if (time_before(rt->time, use_entry->time))
			use_entry = rt;

		/*
		 * Initially, no need to check any further as this entry as well
		 * as following entries have never been used.
		 */
		if (rt->time == 0)
			break;

		/* Check if entry expired. */
		if (time_before(rt->time, invalid_before))
			continue; /* before KCSAN_REPORT_ONCE_IN_MS ago */

		/* Reported recently, check if data race matches. */
		if ((rt->frame1 == frame1 && rt->frame2 == frame2) ||
		    (rt->frame1 == frame2 && rt->frame2 == frame1))
			return true;
	}

	use_entry->time = jiffies;
	use_entry->frame1 = frame1;
	use_entry->frame2 = frame2;
	return false;
}

/*
 * Special rules to skip reporting.
 */
static bool
skip_report(bool value_change, unsigned long top_frame)
{
	/*
	 * The first call to skip_report always has value_change==true, since we
	 * cannot know the value written of an instrumented access. For the 2nd
	 * call there are 6 cases with CONFIG_KCSAN_REPORT_VALUE_CHANGE_ONLY:
	 *
	 * 1. read watchpoint, conflicting write (value_change==true): report;
	 * 2. read watchpoint, conflicting write (value_change==false): skip;
	 * 3. write watchpoint, conflicting write (value_change==true): report;
	 * 4. write watchpoint, conflicting write (value_change==false): skip;
	 * 5. write watchpoint, conflicting read (value_change==false): skip;
	 * 6. write watchpoint, conflicting read (value_change==true): impossible;
	 *
	 * Cases 1-4 are intuitive and expected; case 5 ensures we do not report
	 * data races where the write may have rewritten the same value; and
	 * case 6 is simply impossible.
	 */
	if (IS_ENABLED(CONFIG_KCSAN_REPORT_VALUE_CHANGE_ONLY) && !value_change) {
		/*
		 * The access is a write, but the data value did not change.
		 *
		 * We opt-out of this filter for certain functions at request of
		 * maintainers.
		 */
		char buf[64];

		snprintf(buf, sizeof(buf), "%ps", (void *)top_frame);
		if (!strnstr(buf, "rcu_", sizeof(buf)) &&
		    !strnstr(buf, "_rcu", sizeof(buf)) &&
		    !strnstr(buf, "_srcu", sizeof(buf)))
			return true;
	}

	return kcsan_skip_report_debugfs(top_frame);
}

static const char *get_access_type(int type)
{
	switch (type) {
	case 0:
		return "read";
	case KCSAN_ACCESS_ATOMIC:
		return "read (marked)";
	case KCSAN_ACCESS_WRITE:
		return "write";
	case KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ATOMIC:
		return "write (marked)";
	default:
		BUG();
	}
}

/* Return thread description: in task or interrupt. */
static const char *get_thread_desc(int task_id)
{
	if (task_id != -1) {
		static char buf[32]; /* safe: protected by report_lock */

		snprintf(buf, sizeof(buf), "task %i", task_id);
		return buf;
	}
	return "interrupt";
}

/* Helper to skip KCSAN-related functions in stack-trace. */
static int get_stack_skipnr(unsigned long stack_entries[], int num_entries)
{
	char buf[64];
	int skip = 0;

	for (; skip < num_entries; ++skip) {
		snprintf(buf, sizeof(buf), "%ps", (void *)stack_entries[skip]);
		if (!strnstr(buf, "csan_", sizeof(buf)) &&
		    !strnstr(buf, "tsan_", sizeof(buf)) &&
		    !strnstr(buf, "_once_size", sizeof(buf))) {
			break;
		}
	}
	return skip;
}

/* Compares symbolized strings of addr1 and addr2. */
static int sym_strcmp(void *addr1, void *addr2)
{
	char buf1[64];
	char buf2[64];

	snprintf(buf1, sizeof(buf1), "%pS", addr1);
	snprintf(buf2, sizeof(buf2), "%pS", addr2);

	return strncmp(buf1, buf2, sizeof(buf1));
}

/*
 * Returns true if a report was generated, false otherwise.
 */
static bool print_report(const volatile void *ptr, size_t size, int access_type,
			 bool value_change, int cpu_id,
			 enum kcsan_report_type type)
{
	unsigned long stack_entries[NUM_STACK_ENTRIES] = { 0 };
	int num_stack_entries = stack_trace_save(stack_entries, NUM_STACK_ENTRIES, 1);
	int skipnr = get_stack_skipnr(stack_entries, num_stack_entries);
	unsigned long this_frame = stack_entries[skipnr];
	unsigned long other_frame = 0;
	int other_skipnr = 0; /* silence uninit warnings */

	/*
	 * Must check report filter rules before starting to print.
	 */
	if (skip_report(true, stack_entries[skipnr]))
		return false;

	if (type == KCSAN_REPORT_RACE_SIGNAL) {
		other_skipnr = get_stack_skipnr(other_info.stack_entries,
						other_info.num_stack_entries);
		other_frame = other_info.stack_entries[other_skipnr];

		/* @value_change is only known for the other thread */
		if (skip_report(value_change, other_frame))
			return false;
	}

	if (rate_limit_report(this_frame, other_frame))
		return false;

	/* Print report header. */
	pr_err("==================================================================\n");
	switch (type) {
	case KCSAN_REPORT_RACE_SIGNAL: {
		int cmp;

		/*
		 * Order functions lexographically for consistent bug titles.
		 * Do not print offset of functions to keep title short.
		 */
		cmp = sym_strcmp((void *)other_frame, (void *)this_frame);
		pr_err("BUG: KCSAN: data-race in %ps / %ps\n",
		       (void *)(cmp < 0 ? other_frame : this_frame),
		       (void *)(cmp < 0 ? this_frame : other_frame));
	} break;

	case KCSAN_REPORT_RACE_UNKNOWN_ORIGIN:
		pr_err("BUG: KCSAN: data-race in %pS\n", (void *)this_frame);
		break;

	default:
		BUG();
	}

	pr_err("\n");

	/* Print information about the racing accesses. */
	switch (type) {
	case KCSAN_REPORT_RACE_SIGNAL:
		pr_err("%s to 0x%px of %zu bytes by %s on cpu %i:\n",
		       get_access_type(other_info.access_type), other_info.ptr,
		       other_info.size, get_thread_desc(other_info.task_pid),
		       other_info.cpu_id);

		/* Print the other thread's stack trace. */
		stack_trace_print(other_info.stack_entries + other_skipnr,
				  other_info.num_stack_entries - other_skipnr,
				  0);

		pr_err("\n");
		pr_err("%s to 0x%px of %zu bytes by %s on cpu %i:\n",
		       get_access_type(access_type), ptr, size,
		       get_thread_desc(in_task() ? task_pid_nr(current) : -1),
		       cpu_id);
		break;

	case KCSAN_REPORT_RACE_UNKNOWN_ORIGIN:
		pr_err("race at unknown origin, with %s to 0x%px of %zu bytes by %s on cpu %i:\n",
		       get_access_type(access_type), ptr, size,
		       get_thread_desc(in_task() ? task_pid_nr(current) : -1),
		       cpu_id);
		break;

	default:
		BUG();
	}
	/* Print stack trace of this thread. */
	stack_trace_print(stack_entries + skipnr, num_stack_entries - skipnr,
			  0);

	/* Print report footer. */
	pr_err("\n");
	pr_err("Reported by Kernel Concurrency Sanitizer on:\n");
	dump_stack_print_info(KERN_DEFAULT);
	pr_err("==================================================================\n");

	return true;
}

static void release_report(unsigned long *flags, enum kcsan_report_type type)
{
	if (type == KCSAN_REPORT_RACE_SIGNAL)
		other_info.ptr = NULL; /* mark for reuse */

	spin_unlock_irqrestore(&report_lock, *flags);
}

/*
 * Depending on the report type either sets other_info and returns false, or
 * acquires the matching other_info and returns true. If other_info is not
 * required for the report type, simply acquires report_lock and returns true.
 */
static bool prepare_report(unsigned long *flags, const volatile void *ptr,
			   size_t size, int access_type, int cpu_id,
			   enum kcsan_report_type type)
{
	if (type != KCSAN_REPORT_CONSUMED_WATCHPOINT &&
	    type != KCSAN_REPORT_RACE_SIGNAL) {
		/* other_info not required; just acquire report_lock */
		spin_lock_irqsave(&report_lock, *flags);
		return true;
	}

retry:
	spin_lock_irqsave(&report_lock, *flags);

	switch (type) {
	case KCSAN_REPORT_CONSUMED_WATCHPOINT:
		if (other_info.ptr != NULL)
			break; /* still in use, retry */

		other_info.ptr			= ptr;
		other_info.size			= size;
		other_info.access_type		= access_type;
		other_info.task_pid		= in_task() ? task_pid_nr(current) : -1;
		other_info.cpu_id		= cpu_id;
		other_info.num_stack_entries	= stack_trace_save(other_info.stack_entries, NUM_STACK_ENTRIES, 1);

		spin_unlock_irqrestore(&report_lock, *flags);

		/*
		 * The other thread will print the summary; other_info may now
		 * be consumed.
		 */
		return false;

	case KCSAN_REPORT_RACE_SIGNAL:
		if (other_info.ptr == NULL)
			break; /* no data available yet, retry */

		/*
		 * First check if this is the other_info we are expecting, i.e.
		 * matches based on how watchpoint was encoded.
		 */
		if (!matching_access((unsigned long)other_info.ptr &
					     WATCHPOINT_ADDR_MASK,
				     other_info.size,
				     (unsigned long)ptr & WATCHPOINT_ADDR_MASK,
				     size))
			break; /* mismatching watchpoint, retry */

		if (!matching_access((unsigned long)other_info.ptr,
				     other_info.size, (unsigned long)ptr,
				     size)) {
			/*
			 * If the actual accesses to not match, this was a false
			 * positive due to watchpoint encoding.
			 */
			kcsan_counter_inc(
				KCSAN_COUNTER_ENCODING_FALSE_POSITIVES);

			/* discard this other_info */
			release_report(flags, KCSAN_REPORT_RACE_SIGNAL);
			return false;
		}

		/*
		 * Matching & usable access in other_info: keep other_info_lock
		 * locked, as this thread consumes it to print the full report;
		 * unlocked in release_report.
		 */
		return true;

	default:
		BUG();
	}

	spin_unlock_irqrestore(&report_lock, *flags);

	goto retry;
}

void kcsan_report(const volatile void *ptr, size_t size, int access_type,
		  bool value_change, int cpu_id, enum kcsan_report_type type)
{
	unsigned long flags = 0;

	/*
	 * With TRACE_IRQFLAGS, lockdep's IRQ trace state becomes corrupted if
	 * we do not turn off lockdep here; this could happen due to recursion
	 * into lockdep via KCSAN if we detect a data race in utilities used by
	 * lockdep.
	 */
	lockdep_off();

	kcsan_disable_current();
	if (prepare_report(&flags, ptr, size, access_type, cpu_id, type)) {
		if (print_report(ptr, size, access_type, value_change, cpu_id, type) && panic_on_warn)
			panic("panic_on_warn set ...\n");

		release_report(&flags, type);
	}
	kcsan_enable_current();

	lockdep_on();
}
