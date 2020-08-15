/*
 * Fast batching percpu counters.
 */

#include <linux/percpu_counter.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/debugobjects.h>
#include <linux/slab.h>

#ifdef CONFIG_HOTPLUG_CPU
static LIST_HEAD(percpu_counters);
static DEFINE_SPINLOCK(percpu_counters_lock);
#endif

#ifdef CONFIG_DEBUG_OBJECTS_PERCPU_COUNTER

static struct debug_obj_descr percpu_counter_debug_descr;

static bool percpu_counter_fixup_free(void *addr, enum debug_obj_state state)
{
	struct percpu_counter *fbc = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		percpu_counter_destroy(fbc);
		debug_object_free(fbc, &percpu_counter_debug_descr);
		return true;
	default:
		return false;
	}
}

static struct debug_obj_descr percpu_counter_debug_descr = {
	.name		= "percpu_counter",
	.fixup_free	= percpu_counter_fixup_free,
};

static inline void debug_percpu_counter_activate(struct percpu_counter *fbc)
{
	debug_object_init(fbc, &percpu_counter_debug_descr);
	debug_object_activate(fbc, &percpu_counter_debug_descr);
}

static inline void debug_percpu_counter_deactivate(struct percpu_counter *fbc)
{
	debug_object_deactivate(fbc, &percpu_counter_debug_descr);
	debug_object_free(fbc, &percpu_counter_debug_descr);
}

#else	/* CONFIG_DEBUG_OBJECTS_PERCPU_COUNTER */
static inline void debug_percpu_counter_activate(struct percpu_counter *fbc)
{ }
static inline void debug_percpu_counter_deactivate(struct percpu_counter *fbc)
{ }
#endif	/* CONFIG_DEBUG_OBJECTS_PERCPU_COUNTER */

void percpu_counter_set(struct percpu_counter *fbc, s64 amount)
{
	int cpu;
	struct percpu_counter_rw *pcrw = fbc->pcrw;

	for_each_possible_cpu(cpu) {
		s32 *pcount = per_cpu_ptr(fbc->counters, cpu);
		*pcount = 0;
	}
	atomic64_set(&pcrw->count, amount);
	atomic64_set(&pcrw->slowcount, 0);
}
EXPORT_SYMBOL(percpu_counter_set);

void __percpu_counter_add(struct percpu_counter *fbc, s64 amount, s32 batch)
{
	s64 count;
	struct percpu_counter_rw *pcrw = fbc->pcrw;

	if (atomic_read(&fbc->sum_cnt)) {
		atomic64_add(amount, &pcrw->slowcount);
		return;
	}

	preempt_disable();
	count = __this_cpu_read(*fbc->counters) + amount;
	if (count >= batch || count <= -batch) {
		atomic64_add(count, &pcrw->count);
		pcrw->sequence++;
		__this_cpu_sub(*fbc->counters, count - amount);
	} else {
		this_cpu_add(*fbc->counters, amount);
	}
	preempt_enable();
}
EXPORT_SYMBOL(__percpu_counter_add);

/*
 * Add up all the per-cpu counts, return the result.  This is a more accurate
 * but much slower version of percpu_counter_read_positive()
 */
s64 __percpu_counter_sum(struct percpu_counter *fbc)
{
	s64 ret;
	int cpu;
	unsigned int seq;
	struct percpu_counter_rw *pcrw = fbc->pcrw;

	atomic_inc(&fbc->sum_cnt);
	do {
		seq = pcrw->sequence;
		smp_rmb();

		ret = atomic64_read(&pcrw->count);
		for_each_online_cpu(cpu) {
			s32 *pcount = per_cpu_ptr(fbc->counters, cpu);
			ret += *pcount;
		}

		smp_rmb();
	} while (pcrw->sequence != seq);

	atomic_dec(&fbc->sum_cnt);
	ret += atomic64_read(&pcrw->slowcount);

	return ret;
}
EXPORT_SYMBOL(__percpu_counter_sum);

int __percpu_counter_init(struct percpu_counter *fbc, s64 amount, gfp_t gfp,
			  struct lock_class_key *key)
{
	unsigned long flags __maybe_unused;
	struct percpu_counter_rw *pcrw; 

	pcrw = kzalloc(sizeof(*pcrw), GFP_KERNEL);
	if (!pcrw)
		return -ENOMEM;
	atomic64_set(&pcrw->count, amount);

	fbc->counters = alloc_percpu_gfp(s32, gfp);
	if (!fbc->counters) {
		kfree(pcrw);
		return -ENOMEM;
	}
	
	fbc->pcrw = pcrw;
	atomic_set(&fbc->sum_cnt, 0);

	debug_percpu_counter_activate(fbc);
	
#ifdef CONFIG_HOTPLUG_CPU
	INIT_LIST_HEAD(&pcrw->list);
	pcrw->fbc = fbc;
	spin_lock_irqsave(&percpu_counters_lock, flags);
	list_add(&pcrw->list, &percpu_counters);
	spin_unlock_irqrestore(&percpu_counters_lock, flags);
#endif
	return 0;
}
EXPORT_SYMBOL(__percpu_counter_init);

void percpu_counter_destroy(struct percpu_counter *fbc)
{
	unsigned long flags __maybe_unused;

	if (!fbc->counters)
		return;

	debug_percpu_counter_deactivate(fbc);

#ifdef CONFIG_HOTPLUG_CPU
	spin_lock_irqsave(&percpu_counters_lock, flags);
	list_del(&fbc->pcrw->list);
	spin_unlock_irqrestore(&percpu_counters_lock, flags);
#endif
	free_percpu(fbc->counters);
	fbc->counters = NULL;
	kfree(fbc->pcrw);
	fbc->pcrw = NULL;
}
EXPORT_SYMBOL(percpu_counter_destroy);

int percpu_counter_batch __read_mostly = 32;
EXPORT_SYMBOL(percpu_counter_batch);

static void compute_batch_value(void)
{
	int nr = num_online_cpus();

	percpu_counter_batch = max(32, nr*2);
}

static int percpu_counter_hotcpu_callback(struct notifier_block *nb,
					unsigned long action, void *hcpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	unsigned int cpu;
	struct percpu_counter_rw *pcrw;

	compute_batch_value();
	if (action != CPU_DEAD && action != CPU_DEAD_FROZEN)
		return NOTIFY_OK;

	cpu = (unsigned long)hcpu;
	spin_lock_irq(&percpu_counters_lock);
	list_for_each_entry(pcrw, &percpu_counters, list) {
		s32 *pcount;
		unsigned long flags;

		pcount = per_cpu_ptr(pcrw->fbc->counters, cpu);
		atomic64_add(*pcount, &pcrw->count);
		*pcount = 0;
	}
	spin_unlock_irq(&percpu_counters_lock);
#endif
	return NOTIFY_OK;
}

/*
 * Compare counter against given value.
 * Return 1 if greater, 0 if equal and -1 if less
 */
int __percpu_counter_compare(struct percpu_counter *fbc, s64 rhs, s32 batch)
{
	s64	count;

	count = percpu_counter_read(fbc);
	/* Check to see if rough count will be sufficient for comparison */
	if (abs(count - rhs) > (batch * num_online_cpus())) {
		if (count > rhs)
			return 1;
		else
			return -1;
	}
	/* Need to use precise count */
	count = percpu_counter_sum(fbc);
	if (count > rhs)
		return 1;
	else if (count < rhs)
		return -1;
	else
		return 0;
}
EXPORT_SYMBOL(__percpu_counter_compare);

static int __init percpu_counter_startup(void)
{
	compute_batch_value();
	hotcpu_notifier(percpu_counter_hotcpu_callback, 0);
	return 0;
}
module_init(percpu_counter_startup);
