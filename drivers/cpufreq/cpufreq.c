/*
 *  linux/drivers/cpufreq/cpufreq.c
 *
 *  Copyright (C) 2001 Russell King
 *            (C) 2002 - 2003 Dominik Brodowski <linux@brodo.de>
 *
 *  Oct 2005 - Ashok Raj <ashok.raj@intel.com>
 *	Added handling for CPU hotplug
 *  Feb 2006 - Jacob Shin <jacob.shin@amd.com>
 *	Fix handling for CPU hotplug -- affected CPUs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/syscore_ops.h>
#include <linux/kernel_stat.h>
#include <linux/tick.h>
#include <linux/pm_qos.h>

#include <mach/cpufreq.h>

#include <trace/events/power.h>

/**
 * The "cpufreq driver" - the arch- or hardware-dependent low
 * level driver of CPUFreq support, and its spinlock. This lock
 * also protects the cpufreq_cpu_data array.
 */
static struct cpufreq_driver *cpufreq_driver;
static DEFINE_PER_CPU(struct cpufreq_policy *, cpufreq_cpu_data);
#ifdef CONFIG_HOTPLUG_CPU
/* This one keeps track of the previously set governor of a removed CPU */
struct cpufreq_cpu_save_data {
	char gov[CPUFREQ_NAME_LEN];
	unsigned int max, min;
};
static DEFINE_PER_CPU(struct cpufreq_cpu_save_data, cpufreq_policy_save);
#endif
static DEFINE_SPINLOCK(cpufreq_driver_lock);

/*
 * cpu_policy_rwsem is a per CPU reader-writer semaphore designed to cure
 * all cpufreq/hotplug/workqueue/etc related lock issues.
 *
 * The rules for this semaphore:
 * - Any routine that wants to read from the policy structure will
 *   do a down_read on this semaphore.
 * - Any routine that will write to the policy structure and/or may take away
 *   the policy altogether (eg. CPU hotplug), will hold this lock in write
 *   mode before doing so.
 *
 * Additional rules:
 * - All holders of the lock should check to make sure that the CPU they
 *   are concerned with are online after they get the lock.
 * - Governor routines that can be called in cpufreq hotplug path should not
 *   take this sem as top level hotplug notifier handler takes this.
 * - Lock should not be held across
 *     __cpufreq_governor(data, CPUFREQ_GOV_STOP);
 */
static DEFINE_PER_CPU(int, cpufreq_policy_cpu);
static DEFINE_PER_CPU(struct rw_semaphore, cpu_policy_rwsem);

#define lock_policy_rwsem(mode, cpu)					\
int lock_policy_rwsem_##mode						\
(int cpu)								\
{									\
	int policy_cpu = per_cpu(cpufreq_policy_cpu, cpu);		\
	BUG_ON(policy_cpu == -1);					\
	down_##mode(&per_cpu(cpu_policy_rwsem, policy_cpu));		\
	if (unlikely(!cpu_online(cpu))) {				\
		up_##mode(&per_cpu(cpu_policy_rwsem, policy_cpu));	\
		return -1;						\
	}								\
									\
	return 0;							\
}

lock_policy_rwsem(read, cpu);

lock_policy_rwsem(write, cpu);

static void unlock_policy_rwsem_read(int cpu)
{
	int policy_cpu = per_cpu(cpufreq_policy_cpu, cpu);
	BUG_ON(policy_cpu == -1);
	up_read(&per_cpu(cpu_policy_rwsem, policy_cpu));
}

void unlock_policy_rwsem_write(int cpu)
{
	int policy_cpu = per_cpu(cpufreq_policy_cpu, cpu);
	BUG_ON(policy_cpu == -1);
	up_write(&per_cpu(cpu_policy_rwsem, policy_cpu));
}


/* internal prototypes */
static int __cpufreq_governor(struct cpufreq_policy *policy,
		unsigned int event);
static unsigned int __cpufreq_get(unsigned int cpu);
static void handle_update(struct work_struct *work);

/**
 * Two notifier lists: the "policy" list is involved in the
 * validation process for a new CPU frequency policy; the
 * "transition" list for kernel code that needs to handle
 * changes to devices when the CPU clock speed changes.
 * The mutex locks both lists.
 */
static BLOCKING_NOTIFIER_HEAD(cpufreq_policy_notifier_list);
static struct srcu_notifier_head cpufreq_transition_notifier_list;

static bool init_cpufreq_transition_notifier_list_called;
static int __init init_cpufreq_transition_notifier_list(void)
{
	srcu_init_notifier_head(&cpufreq_transition_notifier_list);
	init_cpufreq_transition_notifier_list_called = true;
	return 0;
}
pure_initcall(init_cpufreq_transition_notifier_list);

static int off __read_mostly;
static int cpufreq_disabled(void)
{
	return off;
}
void disable_cpufreq(void)
{
	off = 1;
}
static LIST_HEAD(cpufreq_governor_list);
static DEFINE_MUTEX(cpufreq_governor_mutex);

static inline u64 get_cpu_idle_time_jiffy(unsigned int cpu, u64 *wall)
{
        u64 idle_time;
        u64 cur_wall_time;
        u64 busy_time;

        cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

        busy_time = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
        busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
        busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
        busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
        busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
        busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

        idle_time = cur_wall_time - busy_time;
        if (wall)
                *wall = cputime_to_usecs(cur_wall_time);

        return cputime_to_usecs(idle_time);
}

u64 get_cpu_idle_time(unsigned int cpu, u64 *wall, int io_busy)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, io_busy ? wall : NULL);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);
	else if (!io_busy)
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}
EXPORT_SYMBOL_GPL(get_cpu_idle_time);

static struct cpufreq_policy *__cpufreq_cpu_get(unsigned int cpu, int sysfs)
{
	struct cpufreq_policy *data;
	unsigned long flags;

	if (cpu >= nr_cpu_ids)
		goto err_out;

	/* get the cpufreq driver */
	spin_lock_irqsave(&cpufreq_driver_lock, flags);

	if (!cpufreq_driver)
		goto err_out_unlock;

	if (!try_module_get(cpufreq_driver->owner))
		goto err_out_unlock;


	/* get the CPU */
	data = per_cpu(cpufreq_cpu_data, cpu);

	if (!data)
		goto err_out_put_module;

	if (!sysfs && !kobject_get(&data->kobj))
		goto err_out_put_module;

	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
	return data;

err_out_put_module:
	module_put(cpufreq_driver->owner);
err_out_unlock:
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
err_out:
	return NULL;
}

struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu)
{
	return __cpufreq_cpu_get(cpu, 0);
}
EXPORT_SYMBOL_GPL(cpufreq_cpu_get);

static struct cpufreq_policy *cpufreq_cpu_get_sysfs(unsigned int cpu)
{
	return __cpufreq_cpu_get(cpu, 1);
}

static void __cpufreq_cpu_put(struct cpufreq_policy *data, int sysfs)
{
	if (!sysfs)
		kobject_put(&data->kobj);
	module_put(cpufreq_driver->owner);
}

void cpufreq_cpu_put(struct cpufreq_policy *data)
{
	__cpufreq_cpu_put(data, 0);
}
EXPORT_SYMBOL_GPL(cpufreq_cpu_put);

static void cpufreq_cpu_put_sysfs(struct cpufreq_policy *data)
{
	__cpufreq_cpu_put(data, 1);
}

/*********************************************************************
 *            EXTERNALLY AFFECTING FREQUENCY CHANGES                 *
 *********************************************************************/

/**
 * adjust_jiffies - adjust the system "loops_per_jiffy"
 *
 * This function alters the system "loops_per_jiffy" for the clock
 * speed change. Note that loops_per_jiffy cannot be updated on SMP
 * systems as each CPU might be scaled differently. So, use the arch
 * per-CPU loops_per_jiffy value wherever possible.
 */
#ifndef CONFIG_SMP
static unsigned long l_p_j_ref;
static unsigned int  l_p_j_ref_freq;

static void adjust_jiffies(unsigned long val, struct cpufreq_freqs *ci)
{
	if (ci->flags & CPUFREQ_CONST_LOOPS)
		return;

	if (!l_p_j_ref_freq) {
		l_p_j_ref = loops_per_jiffy;
		l_p_j_ref_freq = ci->old;
		pr_debug("saving %lu as reference value for loops_per_jiffy; "
			"freq is %u kHz\n", l_p_j_ref, l_p_j_ref_freq);
	}
	if ((val == CPUFREQ_POSTCHANGE  && ci->old != ci->new) ||
	    (val == CPUFREQ_RESUMECHANGE || val == CPUFREQ_SUSPENDCHANGE)) {
		loops_per_jiffy = cpufreq_scale(l_p_j_ref, l_p_j_ref_freq,
								ci->new);
		pr_debug("scaling loops_per_jiffy to %lu "
			"for frequency %u kHz\n", loops_per_jiffy, ci->new);
	}
}
#else
static inline void adjust_jiffies(unsigned long val, struct cpufreq_freqs *ci)
{
	return;
}
#endif


/**
 * cpufreq_notify_transition - call notifier chain and adjust_jiffies
 * on frequency transition.
 *
 * This function calls the transition notifiers and the "adjust_jiffies"
 * function. It is called twice on all CPU frequency changes that have
 * external effects.
 */
void cpufreq_notify_transition(struct cpufreq_freqs *freqs, unsigned int state)
{
	struct cpufreq_policy *policy;

	BUG_ON(irqs_disabled());

	freqs->flags = cpufreq_driver->flags;
	pr_debug("notification %u of frequency transition to %u kHz\n",
		state, freqs->new);

	policy = per_cpu(cpufreq_cpu_data, freqs->cpu);
	switch (state) {

	case CPUFREQ_PRECHANGE:
		/* detect if the driver reported a value as "old frequency"
		 * which is not equal to what the cpufreq core thinks is
		 * "old frequency".
		 */
		if (!(cpufreq_driver->flags & CPUFREQ_CONST_LOOPS)) {
			if ((policy) && (policy->cpu == freqs->cpu) &&
			    (policy->cur) && (policy->cur != freqs->old)) {
				pr_debug("Warning: CPU frequency is"
					" %u, cpufreq assumed %u kHz.\n",
					freqs->old, policy->cur);
				freqs->old = policy->cur;
			}
		}
		srcu_notifier_call_chain(&cpufreq_transition_notifier_list,
				CPUFREQ_PRECHANGE, freqs);
		adjust_jiffies(CPUFREQ_PRECHANGE, freqs);
		break;

	case CPUFREQ_POSTCHANGE:
		adjust_jiffies(CPUFREQ_POSTCHANGE, freqs);
		pr_debug("FREQ: %lu - CPU: %lu", (unsigned long)freqs->new,
			(unsigned long)freqs->cpu);
		trace_cpu_frequency(freqs->new, freqs->cpu);
		srcu_notifier_call_chain(&cpufreq_transition_notifier_list,
				CPUFREQ_POSTCHANGE, freqs);
		if (likely(policy) && likely(policy->cpu == freqs->cpu)) {
			policy->cur = freqs->new;
			sysfs_notify(&policy->kobj, NULL, "scaling_cur_freq");
		}
		break;
	}
}
EXPORT_SYMBOL_GPL(cpufreq_notify_transition);
/**
 * cpufreq_notify_utilization - notify CPU userspace about CPU utilization
 * change
 *
 * This function is called everytime the CPU load is evaluated by the
 * ondemand governor. It notifies userspace of cpu load changes via sysfs.
 */
void cpufreq_notify_utilization(struct cpufreq_policy *policy,
		unsigned int util)
{
	if (policy)
		policy->util = util;
}

/*********************************************************************
 *                          SYSFS INTERFACE                          *
 *********************************************************************/

static struct cpufreq_governor *__find_governor(const char *str_governor)
{
	struct cpufreq_governor *t;

	list_for_each_entry(t, &cpufreq_governor_list, governor_list)
		if (!strnicmp(str_governor, t->name, CPUFREQ_NAME_LEN))
			return t;

	return NULL;
}

/**
 * cpufreq_parse_governor - parse a governor string
 */
static int cpufreq_parse_governor(char *str_governor, unsigned int *policy,
				struct cpufreq_governor **governor)
{
	int err = -EINVAL;

	if (!cpufreq_driver)
		goto out;

	if (cpufreq_driver->setpolicy) {
		if (!strnicmp(str_governor, "performance", CPUFREQ_NAME_LEN)) {
			*policy = CPUFREQ_POLICY_PERFORMANCE;
			err = 0;
		} else if (!strnicmp(str_governor, "powersave",
						CPUFREQ_NAME_LEN)) {
			*policy = CPUFREQ_POLICY_POWERSAVE;
			err = 0;
		}
	} else if (cpufreq_driver->target) {
		struct cpufreq_governor *t;

		mutex_lock(&cpufreq_governor_mutex);

		t = __find_governor(str_governor);

		if (t == NULL) {
			int ret;

			mutex_unlock(&cpufreq_governor_mutex);
			ret = request_module("cpufreq_%s", str_governor);
			mutex_lock(&cpufreq_governor_mutex);

			if (ret == 0)
				t = __find_governor(str_governor);
		}

		if (t != NULL) {
			*governor = t;
			err = 0;
		}

		mutex_unlock(&cpufreq_governor_mutex);
	}
out:
	return err;
}

/**
 * cpufreq_per_cpu_attr_read() / show_##file_name() -
 * print out cpufreq information
 *
 * Write out information from cpufreq_driver->policy[cpu]; object must be
 * "unsigned int".
 */

#define show_one(file_name, object)			\
static ssize_t show_##file_name				\
(struct cpufreq_policy *policy, char *buf)		\
{							\
	return sprintf(buf, "%u\n", policy->object);	\
}

show_one(cpuinfo_min_freq, cpuinfo.min_freq);
show_one(cpuinfo_max_freq, cpuinfo.max_freq);
show_one(cpuinfo_transition_latency, cpuinfo.transition_latency);
show_one(scaling_min_freq, min);
show_one(scaling_max_freq, max);
show_one(cpu_utilization, util);
show_one(policy_min_freq, user_policy.min);
show_one(policy_max_freq, user_policy.max);

static ssize_t show_scaling_cur_freq(
	struct cpufreq_policy *policy, char *buf)
{
	ssize_t ret;

	if (cpufreq_driver && cpufreq_driver->setpolicy && cpufreq_driver->get)
		ret = sprintf(buf, "%u\n", cpufreq_driver->get(policy->cpu));
	else
		ret = sprintf(buf, "%u\n", policy->cur);
	return ret;
}

static int __cpufreq_set_policy(struct cpufreq_policy *data,
				struct cpufreq_policy *policy);

/**
 * cpufreq_per_cpu_attr_write() / store_##file_name() - sysfs write access
 */
#define store_one(file_name, object)			\
static ssize_t store_##file_name					\
(struct cpufreq_policy *policy, const char *buf, size_t count)		\
{									\
	unsigned int ret, cpu, limited_cpu_freq;			\
	struct cpufreq_policy new_policy;				\
									\
	ret = cpufreq_get_policy(&new_policy, policy->cpu);		\
	if (ret)							\
		return -EINVAL;						\
									\
	cpu = policy->cpu;						\
	limited_cpu_freq = get_max_lock(cpu);				\
									\
	new_policy.min = new_policy.user_policy.min;			\
	new_policy.max = new_policy.user_policy.max;			\
									\
	ret = sscanf(buf, "%u", &new_policy.object);			\
	if (ret != 1)							\
		return -EINVAL;						\
									\
	ret = cpufreq_driver->verify(&new_policy);			\
	if (ret)							\
		pr_err("cpufreq: Frequency verification failed\n");	\
									\
	if (limited_cpu_freq > 0) {					\
		if (new_policy.max > limited_cpu_freq)			\
			new_policy.max = limited_cpu_freq;		\
	}								\
									\
	policy->user_policy.max = new_policy.max;			\
	policy->user_policy.min = new_policy.min;			\
									\
	ret = __cpufreq_set_policy(policy, &new_policy);		\
	policy->user_policy.object = new_policy.object;			\
									\
	return ret ? ret : count;					\
}

store_one(scaling_min_freq, min);
store_one(scaling_max_freq, max);

#ifdef CONFIG_MULTI_CPU_POLICY_LIMIT
#define show_scaling_freq(file_name, object)			\
static ssize_t show_##file_name				\
(struct kobject *a, struct attribute *b, char *buf)		\
{							\
	struct cpufreq_policy *cpu_policy;	\
	unsigned int freq = 0;		\
								\
	cpu_policy = __cpufreq_cpu_get(0, 1);	\
	if (!cpu_policy)				\
		return -EINVAL;					\
											\
	freq = cpu_policy->object;			\
											\
	__cpufreq_cpu_put(cpu_policy, 1);			\
												\
	return sprintf(buf, "%u\n", freq);	\
}
show_scaling_freq(scaling_min_freq_all_cpus, min);
show_scaling_freq(scaling_max_freq_all_cpus, max);

#define show_pcpu_scaling_freq(file_name, object, num_core)	\
static ssize_t show_##file_name##num_core				\
(struct kobject *a, struct attribute *b, char *buf)			\
{															\
	struct cpufreq_policy *cpu_policy;						\
	unsigned int freq = 0;										\
																\
	get_online_cpus();											\
	if (!cpu_online(num_core)) {								\
		freq = per_cpu(cpufreq_policy_save, num_core).object;	\
	} else {													\
		cpu_policy = __cpufreq_cpu_get(num_core, 1);			\
		if (!cpu_policy) {										\
			put_online_cpus();									\
			return -EINVAL;									\
		}													\
		freq = cpu_policy->object;						\
		__cpufreq_cpu_put(cpu_policy, 1);			\
	}											\
	put_online_cpus();						\
	return sprintf(buf, "%u\n", freq);	\
}
show_pcpu_scaling_freq(scaling_min_freq_cpu, min, 1);
show_pcpu_scaling_freq(scaling_min_freq_cpu, min, 2);
show_pcpu_scaling_freq(scaling_min_freq_cpu, min, 3);
show_pcpu_scaling_freq(scaling_max_freq_cpu, max, 1);
show_pcpu_scaling_freq(scaling_max_freq_cpu, max, 2);
show_pcpu_scaling_freq(scaling_max_freq_cpu, max, 3);

#define store_scaling_freq(file_name, ref_store_name, object)			\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int cpu;					\
	unsigned int freq = 0;					\
	unsigned int ret;						\
											\
	ret = sscanf(buf, "%u", &freq);			\
	if (ret != 1)							\
		return -EINVAL;							\
													\
	get_online_cpus();									\
	for_each_possible_cpu(cpu) {							\
		struct cpufreq_policy *cpu_policy;						\
																\
		if (!cpu_online(cpu)) {									\
			per_cpu(cpufreq_policy_save, cpu).object = freq;	\
			continue;											\
		}														\
		cpu_policy = __cpufreq_cpu_get(cpu, 1);						\
		if (!cpu_policy)											\
			continue;												\
																	\
		ret = store_##ref_store_name(cpu_policy, buf, count);		\
																	\
		__cpufreq_cpu_put(cpu_policy, 1);						\
	}															\
	put_online_cpus();											\
															\
	return count;									\
}
store_scaling_freq(scaling_min_freq_all_cpus, scaling_min_freq, min);
store_scaling_freq(scaling_max_freq_all_cpus, scaling_max_freq, max);

#define store_pcpu_scaling_freq(file_name, ref_store_name, object, num_core)	\
static ssize_t store_##file_name##num_core									\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{										\
	struct cpufreq_policy *cpu_policy;					\
	unsigned int freq = 0;						\
	unsigned int ret;						\
											\
	ret = sscanf(buf, "%u", &freq);			\
	if (ret != 1)							\
		return -EINVAL;							\
													\
	get_online_cpus();									\
	if (!cpu_online(num_core)) {							\
		per_cpu(cpufreq_policy_save, num_core).object = freq;	\
	} else {														\
		cpu_policy = __cpufreq_cpu_get(num_core, 1);				\
		if (!cpu_policy) {										\
			put_online_cpus();									\
			return -EINVAL;											\
		}															\
		ret = store_##ref_store_name(cpu_policy, buf, count);	\
		__cpufreq_cpu_put(cpu_policy, 1);					\
	}													\
	put_online_cpus();								\
	return count;								\
}
store_pcpu_scaling_freq(scaling_min_freq_cpu, scaling_min_freq, min, 1);
store_pcpu_scaling_freq(scaling_min_freq_cpu, scaling_min_freq, min, 2);
store_pcpu_scaling_freq(scaling_min_freq_cpu, scaling_min_freq, min, 3);
store_pcpu_scaling_freq(scaling_max_freq_cpu, scaling_max_freq, max, 1);
store_pcpu_scaling_freq(scaling_max_freq_cpu, scaling_max_freq, max, 2);
store_pcpu_scaling_freq(scaling_max_freq_cpu, scaling_max_freq, max, 3);
#endif

/**
 * show_cpuinfo_cur_freq - current CPU frequency as detected by hardware
 */
static ssize_t show_cpuinfo_cur_freq(struct cpufreq_policy *policy,
					char *buf)
{
	unsigned int cur_freq = __cpufreq_get(policy->cpu);
	if (!cur_freq)
		return sprintf(buf, "<unknown>");
	return sprintf(buf, "%u\n", cur_freq);
}


/**
 * show_scaling_governor - show the current policy for the specified CPU
 */
static ssize_t show_scaling_governor(struct cpufreq_policy *policy, char *buf)
{
	if (policy->policy == CPUFREQ_POLICY_POWERSAVE)
		return sprintf(buf, "powersave\n");
	else if (policy->policy == CPUFREQ_POLICY_PERFORMANCE)
		return sprintf(buf, "performance\n");
	else if (policy->governor)
		return scnprintf(buf, CPUFREQ_NAME_PLEN, "%s\n",
				policy->governor->name);
	return -EINVAL;
}


/**
 * store_scaling_governor - store policy for the specified CPU
 */
static ssize_t store_scaling_governor(struct cpufreq_policy *policy,
					const char *buf, size_t count)
{
	unsigned int ret;
	char	str_governor[16];
	struct cpufreq_policy new_policy;

	ret = cpufreq_get_policy(&new_policy, policy->cpu);
	if (ret)
		return ret;

	ret = sscanf(buf, "%15s", str_governor);
	if (ret != 1)
		return -EINVAL;

	if (cpufreq_parse_governor(str_governor, &new_policy.policy,
						&new_policy.governor))
		return -EINVAL;

	/* Do not use cpufreq_set_policy here or the user_policy.max
	   will be wrongly overridden */
	ret = __cpufreq_set_policy(policy, &new_policy);

	if (policy->max > 2803200)
		policy->max = 2803200;

	policy->user_policy.policy = policy->policy;
	policy->user_policy.governor = policy->governor;

	sysfs_notify(&policy->kobj, NULL, "scaling_governor");

	if (ret)
		return ret;
	else
		return count;
}

#ifdef CONFIG_MULTI_CPU_POLICY_LIMIT
/**
 * show_scaling_governor_all_cpus - show the current policy for the specified CPU
 */
static ssize_t show_scaling_governor_all_cpus(struct kobject *a, struct attribute *b, char *buf)
{
	struct cpufreq_policy *cpu_policy;
	char str_governor[16];

	cpu_policy = __cpufreq_cpu_get(0, 1);
	if (!cpu_policy)
		return -EINVAL;

	if (cpu_policy->policy == CPUFREQ_POLICY_POWERSAVE)
		sprintf(str_governor, "powersave\n");
	else if (cpu_policy->policy == CPUFREQ_POLICY_PERFORMANCE)
		sprintf(str_governor, "performance\n");
	else if (cpu_policy->governor)
		scnprintf(str_governor, CPUFREQ_NAME_LEN, "%s\n",
				cpu_policy->governor->name);

	__cpufreq_cpu_put(cpu_policy, 1);

	return scnprintf(buf, CPUFREQ_NAME_LEN, "%s\n",
				str_governor);
}

#define show_pcpu_scaling_governor(num_core)		\
static ssize_t show_scaling_governor_cpu##num_core				\
(struct kobject *a, struct attribute *b, char *buf)		\
{											\
	struct cpufreq_policy *cpu_policy;			\
	char str_governor[16];						\
													\
	get_online_cpus();										\
	if (!cpu_online(num_core)) {										\
		strncpy(str_governor, per_cpu(cpufreq_policy_save, num_core).gov,	\
				CPUFREQ_NAME_LEN);												\
	} else {																	\
		cpu_policy = __cpufreq_cpu_get(num_core, 1);					\
		if (!cpu_policy) {										\
			put_online_cpus();									\
			return -EINVAL;											\
		}															\
																\
		if (cpu_policy->policy == CPUFREQ_POLICY_POWERSAVE)		\
			sprintf(str_governor, "powersave\n");					\
		else if (cpu_policy->policy == CPUFREQ_POLICY_PERFORMANCE)	\
			sprintf(str_governor, "performance\n");					\
		else if (cpu_policy->governor)								\
			scnprintf(str_governor, CPUFREQ_NAME_LEN, "%s\n",	\
					cpu_policy->governor->name);			\
													\
		__cpufreq_cpu_put(cpu_policy, 1);		\
	}										\
	put_online_cpus();					\
													\
	return scnprintf(buf, CPUFREQ_NAME_LEN, "%s\n",		\
				str_governor);						\
}
show_pcpu_scaling_governor(1);
show_pcpu_scaling_governor(2);
show_pcpu_scaling_governor(3);

/**
 * store_scaling_governor_all_cpus - store policy governor for the all CPUs
 */
static ssize_t store_scaling_governor_all_cpus(struct kobject *a, struct attribute *b, const char *buf, size_t count)
{
	char str_governor[16];
	unsigned int cpu;
	unsigned int ret;

	ret = sscanf(buf, "%15s", str_governor);
	if (ret != 1)
		return -EINVAL;

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *cpu_policy;

#ifdef CONFIG_HOTPLUG_CPU
		if (!cpu_online(cpu)) {
			strncpy(per_cpu(cpufreq_policy_save, cpu).gov, str_governor,
				CPUFREQ_NAME_LEN);
			continue;
		}
#endif
		cpu_policy = __cpufreq_cpu_get(cpu, 1);
		if (!cpu_policy)
			continue;

		ret = store_scaling_governor(cpu_policy, buf, count);

		__cpufreq_cpu_put(cpu_policy, 1);
	}
	put_online_cpus();

	return count;
}

#define store_pcpu_scaling_governor(num_core)					\
static ssize_t store_scaling_governor_cpu##num_core					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{														\
	struct cpufreq_policy *cpu_policy;						\
	char str_governor[16];											\
	unsigned int ret;													\
																\
	ret = sscanf(buf, "%15s", str_governor);				\
	if (ret != 1)											\
		return -EINVAL;											\
																	\
	get_online_cpus();													\
	if (!cpu_online(num_core)) {											\
		strncpy(per_cpu(cpufreq_policy_save, num_core).gov, str_governor,		\
			CPUFREQ_NAME_LEN);												\
	} else {															\
		cpu_policy = __cpufreq_cpu_get(num_core, 1);				\
		if (!cpu_policy) {										\
			put_online_cpus();									\
			return -EINVAL;											\
		}															\
		ret = store_scaling_governor(cpu_policy, buf, count);	\
																\
		__cpufreq_cpu_put(cpu_policy, 1);						\
	}														\
	put_online_cpus();						\
									\
	return count;			\
}
store_pcpu_scaling_governor(1);
store_pcpu_scaling_governor(2);
store_pcpu_scaling_governor(3);
#endif

/**
 * show_scaling_driver - show the cpufreq driver currently loaded
 */
static ssize_t show_scaling_driver(struct cpufreq_policy *policy, char *buf)
{
	return scnprintf(buf, CPUFREQ_NAME_PLEN, "%s\n", cpufreq_driver->name);
}

/**
 * show_scaling_available_governors - show the available CPUfreq governors
 */
static ssize_t show_scaling_available_governors(struct cpufreq_policy *policy,
						char *buf)
{
	ssize_t i = 0;
	struct cpufreq_governor *t;

	if (!cpufreq_driver->target) {
		i += sprintf(buf, "performance powersave");
		goto out;
	}

	list_for_each_entry(t, &cpufreq_governor_list, governor_list) {
		if (i >= (ssize_t) ((PAGE_SIZE / sizeof(char))
		    - (CPUFREQ_NAME_LEN + 2)))
			goto out;
		i += scnprintf(&buf[i], CPUFREQ_NAME_PLEN, "%s ", t->name);
	}
out:
	i += sprintf(&buf[i], "\n");
	return i;
}

static ssize_t show_cpus(const struct cpumask *mask, char *buf)
{
	ssize_t i = 0;
	unsigned int cpu;

	for_each_cpu(cpu, mask) {
		if (i)
			i += scnprintf(&buf[i], (PAGE_SIZE - i - 2), " ");
		i += scnprintf(&buf[i], (PAGE_SIZE - i - 2), "%u", cpu);
		if (i >= (PAGE_SIZE - 5))
			break;
	}
	i += sprintf(&buf[i], "\n");
	return i;
}

/**
 * show_related_cpus - show the CPUs affected by each transition even if
 * hw coordination is in use
 */
static ssize_t show_related_cpus(struct cpufreq_policy *policy, char *buf)
{
	if (cpumask_empty(policy->related_cpus))
		return show_cpus(policy->cpus, buf);
	return show_cpus(policy->related_cpus, buf);
}

/**
 * show_affected_cpus - show the CPUs affected by each transition
 */
static ssize_t show_affected_cpus(struct cpufreq_policy *policy, char *buf)
{
	return show_cpus(policy->cpus, buf);
}

static ssize_t store_scaling_setspeed(struct cpufreq_policy *policy,
					const char *buf, size_t count)
{
	unsigned int freq = 0;
	unsigned int ret;

	if (!policy->governor || !policy->governor->store_setspeed)
		return -EINVAL;

	ret = sscanf(buf, "%u", &freq);
	if (ret != 1)
		return -EINVAL;

	policy->governor->store_setspeed(policy, freq);

	return count;
}

static ssize_t show_scaling_setspeed(struct cpufreq_policy *policy, char *buf)
{
	if (!policy->governor || !policy->governor->show_setspeed)
		return sprintf(buf, "<unsupported>\n");

	return policy->governor->show_setspeed(policy, buf);
}

/**
 * show_bios_limit - show the current cpufreq HW/BIOS limitation
 */
static ssize_t show_bios_limit(struct cpufreq_policy *policy, char *buf)
{
	unsigned int limit;
	int ret;
	if (cpufreq_driver->bios_limit) {
		ret = cpufreq_driver->bios_limit(policy->cpu, &limit);
		if (!ret)
			return sprintf(buf, "%u\n", limit);
	}
	return sprintf(buf, "%u\n", policy->cpuinfo.max_freq);
}

cpufreq_freq_attr_ro_perm(cpuinfo_cur_freq, 0400);
cpufreq_freq_attr_ro(cpuinfo_min_freq);
cpufreq_freq_attr_ro(cpuinfo_max_freq);
cpufreq_freq_attr_ro(cpuinfo_transition_latency);
cpufreq_freq_attr_ro(scaling_available_governors);
cpufreq_freq_attr_ro(scaling_driver);
cpufreq_freq_attr_ro(scaling_cur_freq);
cpufreq_freq_attr_ro(bios_limit);
cpufreq_freq_attr_ro(related_cpus);
cpufreq_freq_attr_ro(affected_cpus);
cpufreq_freq_attr_ro(cpu_utilization);
cpufreq_freq_attr_rw(scaling_min_freq);
cpufreq_freq_attr_rw(scaling_max_freq);
cpufreq_freq_attr_rw(scaling_governor);
cpufreq_freq_attr_rw(scaling_setspeed);
cpufreq_freq_attr_ro(policy_min_freq);
cpufreq_freq_attr_ro(policy_max_freq);
#ifdef CONFIG_MULTI_CPU_POLICY_LIMIT
define_one_global_rw(scaling_min_freq_all_cpus);
define_one_global_rw(scaling_max_freq_all_cpus);
define_one_global_rw(scaling_governor_all_cpus);
define_one_global_rw(scaling_min_freq_cpu1);
define_one_global_rw(scaling_min_freq_cpu2);
define_one_global_rw(scaling_min_freq_cpu3);
define_one_global_rw(scaling_max_freq_cpu1);
define_one_global_rw(scaling_max_freq_cpu2);
define_one_global_rw(scaling_max_freq_cpu3);
define_one_global_rw(scaling_governor_cpu1);
define_one_global_rw(scaling_governor_cpu2);
define_one_global_rw(scaling_governor_cpu3);
#endif

static struct attribute *default_attrs[] = {
	&cpuinfo_min_freq.attr,
	&cpuinfo_max_freq.attr,
	&cpuinfo_transition_latency.attr,
	&scaling_min_freq.attr,
	&scaling_max_freq.attr,
	&affected_cpus.attr,
	&cpu_utilization.attr,
	&related_cpus.attr,
	&scaling_governor.attr,
	&scaling_driver.attr,
	&scaling_available_governors.attr,
	&scaling_setspeed.attr,
	&policy_min_freq.attr,
	&policy_max_freq.attr,
	NULL
};

#ifdef CONFIG_MULTI_CPU_POLICY_LIMIT
static struct attribute *all_cpus_attrs[] = {
	&scaling_min_freq_all_cpus.attr,
	&scaling_max_freq_all_cpus.attr,
	&scaling_governor_all_cpus.attr,
	&scaling_min_freq_cpu1.attr,
	&scaling_min_freq_cpu2.attr,
	&scaling_min_freq_cpu3.attr,
	&scaling_max_freq_cpu1.attr,
	&scaling_max_freq_cpu2.attr,
	&scaling_max_freq_cpu3.attr,
	&scaling_governor_cpu1.attr,
	&scaling_governor_cpu2.attr,
	&scaling_governor_cpu3.attr,
	NULL
};

static struct attribute_group all_cpus_attr_group = {
	.attrs = all_cpus_attrs,
	.name = "all_cpus",
};
#endif	/* CONFIG_MULTI_CPU_POLICY_LIMIT */

struct kobject *cpufreq_global_kobject;
EXPORT_SYMBOL(cpufreq_global_kobject);

#define to_policy(k) container_of(k, struct cpufreq_policy, kobj)
#define to_attr(a) container_of(a, struct freq_attr, attr)

static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct cpufreq_policy *policy = to_policy(kobj);
	struct freq_attr *fattr = to_attr(attr);
	ssize_t ret = -EINVAL;
	policy = cpufreq_cpu_get_sysfs(policy->cpu);
	if (!policy)
		goto no_policy;

	if (lock_policy_rwsem_read(policy->cpu) < 0)
		goto fail;

	if (fattr->show)
		ret = fattr->show(policy, buf);
	else
		ret = -EIO;

	unlock_policy_rwsem_read(policy->cpu);
fail:
	cpufreq_cpu_put_sysfs(policy);
no_policy:
	return ret;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		     const char *buf, size_t count)
{
	struct cpufreq_policy *policy = to_policy(kobj);
	struct freq_attr *fattr = to_attr(attr);
	ssize_t ret = -EINVAL;
	policy = cpufreq_cpu_get_sysfs(policy->cpu);
	if (!policy)
		goto no_policy;

	if (lock_policy_rwsem_write(policy->cpu) < 0)
		goto fail;

	if (fattr->store)
		ret = fattr->store(policy, buf, count);
	else
		ret = -EIO;

	unlock_policy_rwsem_write(policy->cpu);
fail:
	cpufreq_cpu_put_sysfs(policy);
no_policy:
	return ret;
}

static void cpufreq_sysfs_release(struct kobject *kobj)
{
	struct cpufreq_policy *policy = to_policy(kobj);
	pr_debug("last reference is dropped\n");
	complete(&policy->kobj_unregister);
}

static const struct sysfs_ops sysfs_ops = {
	.show	= show,
	.store	= store,
};

static struct kobj_type ktype_cpufreq = {
	.sysfs_ops	= &sysfs_ops,
	.default_attrs	= default_attrs,
	.release	= cpufreq_sysfs_release,
};

/*
 * Returns:
 *   Negative: Failure
 *   0:        Success
 *   Positive: When we have a managed CPU and the sysfs got symlinked
 */
static int cpufreq_add_dev_policy(unsigned int cpu,
				  struct cpufreq_policy *policy,
				  struct device *dev)
{
	int ret = 0;
#ifdef CONFIG_SMP
	unsigned long flags;
	unsigned int j;
#ifdef CONFIG_HOTPLUG_CPU
	struct cpufreq_governor *gov;

	gov = __find_governor(per_cpu(cpufreq_policy_save, cpu).gov);
	if (gov) {
		policy->governor = gov;
		pr_debug("Restoring governor %s for cpu %d\n",
		       policy->governor->name, cpu);
	}
	if (per_cpu(cpufreq_policy_save, cpu).min) {
		policy->min = per_cpu(cpufreq_policy_save, cpu).min;
		policy->user_policy.min = policy->min;
	}
	if (per_cpu(cpufreq_policy_save, cpu).max) {
		policy->max = per_cpu(cpufreq_policy_save, cpu).max;
		policy->user_policy.max = policy->max;
	}
	pr_debug("Restoring CPU%d min %d and max %d\n",
		cpu, policy->min, policy->max);
#endif

	for_each_cpu(j, policy->cpus) {
		struct cpufreq_policy *managed_policy;

		if (cpu == j)
			continue;

		/* Check for existing affected CPUs.
		 * They may not be aware of it due to CPU Hotplug.
		 * cpufreq_cpu_put is called when the device is removed
		 * in __cpufreq_remove_dev()
		 */
		managed_policy = cpufreq_cpu_get(j);
		if (unlikely(managed_policy)) {

			/* Set proper policy_cpu */
			unlock_policy_rwsem_write(cpu);
			per_cpu(cpufreq_policy_cpu, cpu) = managed_policy->cpu;

			if (lock_policy_rwsem_write(cpu) < 0) {
				/* Should not go through policy unlock path */
				if (cpufreq_driver->exit)
					cpufreq_driver->exit(policy);
				cpufreq_cpu_put(managed_policy);
				return -EBUSY;
			}

			__cpufreq_governor(managed_policy, CPUFREQ_GOV_STOP);

			spin_lock_irqsave(&cpufreq_driver_lock, flags);
			cpumask_copy(managed_policy->cpus, policy->cpus);
			cpumask_and(managed_policy->cpus,
					managed_policy->cpus, cpu_online_mask);
			per_cpu(cpufreq_cpu_data, cpu) = managed_policy;
			spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

			__cpufreq_governor(managed_policy, CPUFREQ_GOV_START);
			__cpufreq_governor(managed_policy, CPUFREQ_GOV_LIMITS);

			pr_debug("CPU already managed, adding link\n");
			ret = sysfs_create_link(&dev->kobj,
						&managed_policy->kobj,
						"cpufreq");
			if (ret)
				cpufreq_cpu_put(managed_policy);
			/*
			 * Success. We only needed to be added to the mask.
			 * Call driver->exit() because only the cpu parent of
			 * the kobj needed to call init().
			 */
			if (cpufreq_driver->exit)
				cpufreq_driver->exit(policy);

			if (!ret)
				return 1;
			else
				return ret;
		}
	}
#endif
	return ret;
}


/* symlink affected CPUs */
static int cpufreq_add_dev_symlink(unsigned int cpu,
				   struct cpufreq_policy *policy)
{
	unsigned int j;
	int ret = 0;

	for_each_cpu(j, policy->cpus) {
		struct cpufreq_policy *managed_policy;
		struct device *cpu_dev;

		if (j == cpu)
			continue;
		if (!cpu_online(j))
			continue;

		pr_debug("CPU %u already managed, adding link\n", j);
		managed_policy = cpufreq_cpu_get(cpu);
		cpu_dev = get_cpu_device(j);
		ret = sysfs_create_link(&cpu_dev->kobj, &policy->kobj,
					"cpufreq");
		if (ret) {
			cpufreq_cpu_put(managed_policy);
			return ret;
		}
	}
	return ret;
}

static int cpufreq_add_dev_interface(unsigned int cpu,
				     struct cpufreq_policy *policy,
				     struct device *dev)
{
	struct cpufreq_policy new_policy;
	struct freq_attr **drv_attr;
	unsigned long flags;
	int ret = 0;
	unsigned int j;

	/* prepare interface data */
	ret = kobject_init_and_add(&policy->kobj, &ktype_cpufreq,
				   &dev->kobj, "cpufreq");
	if (ret)
		return ret;

	/* set up files for this cpu device */
	drv_attr = cpufreq_driver->attr;
	while ((drv_attr) && (*drv_attr)) {
		ret = sysfs_create_file(&policy->kobj, &((*drv_attr)->attr));
		if (ret)
			goto err_out_kobj_put;
		drv_attr++;
	}
	if (cpufreq_driver->get) {
		ret = sysfs_create_file(&policy->kobj, &cpuinfo_cur_freq.attr);
		if (ret)
			goto err_out_kobj_put;
	}

	ret = sysfs_create_file(&policy->kobj, &scaling_cur_freq.attr);
	if (ret)
		goto err_out_kobj_put;

	if (cpufreq_driver->bios_limit) {
		ret = sysfs_create_file(&policy->kobj, &bios_limit.attr);
		if (ret)
			goto err_out_kobj_put;
	}

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	for_each_cpu(j, policy->cpus) {
		if (!cpu_online(j))
			continue;
		per_cpu(cpufreq_cpu_data, j) = policy;
		per_cpu(cpufreq_policy_cpu, j) = policy->cpu;
	}
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	ret = cpufreq_add_dev_symlink(cpu, policy);
	if (ret)
		goto err_out_kobj_put;

	memcpy(&new_policy, policy, sizeof(struct cpufreq_policy));
	/* assure that the starting sequence is run in __cpufreq_set_policy */
	policy->governor = NULL;

	/* set default policy */
	ret = __cpufreq_set_policy(policy, &new_policy);
	policy->user_policy.policy = policy->policy;
	policy->user_policy.governor = policy->governor;

	if (ret) {
		pr_debug("setting policy failed\n");
		if (cpufreq_driver->exit)
			cpufreq_driver->exit(policy);
	}
	return ret;

err_out_kobj_put:
	kobject_put(&policy->kobj);
	wait_for_completion(&policy->kobj_unregister);
	return ret;
}


/**
 * cpufreq_add_dev - add a CPU device
 *
 * Adds the cpufreq interface for a CPU device.
 *
 * The Oracle says: try running cpufreq registration/unregistration concurrently
 * with with cpu hotplugging and all hell will break loose. Tried to clean this
 * mess up, but more thorough testing is needed. - Mathieu
 */
static int cpufreq_add_dev(struct device *dev, struct subsys_interface *sif)
{
	unsigned int cpu = dev->id;
	int ret = 0, found = 0;
	struct cpufreq_policy *policy;
	unsigned long flags;
	unsigned int j;
#ifdef CONFIG_HOTPLUG_CPU
	int sibling;
#endif

	if (cpu_is_offline(cpu))
		return 0;

	pr_debug("adding CPU %u\n", cpu);

#ifdef CONFIG_SMP
	/* check whether a different CPU already registered this
	 * CPU because it is in the same boat. */
	policy = cpufreq_cpu_get(cpu);
	if (unlikely(policy)) {
		cpufreq_cpu_put(policy);
		return 0;
	}
#endif

	if (!try_module_get(cpufreq_driver->owner)) {
		ret = -EINVAL;
		goto module_out;
	}

	ret = -ENOMEM;
	policy = kzalloc(sizeof(struct cpufreq_policy), GFP_KERNEL);
	if (!policy)
		goto nomem_out;

	if (!alloc_cpumask_var(&policy->cpus, GFP_KERNEL))
		goto err_free_policy;

	if (!zalloc_cpumask_var(&policy->related_cpus, GFP_KERNEL))
		goto err_free_cpumask;

	policy->cpu = cpu;
	cpumask_copy(policy->cpus, cpumask_of(cpu));

	/* Initially set CPU itself as the policy_cpu */
	per_cpu(cpufreq_policy_cpu, cpu) = cpu;
	ret = (lock_policy_rwsem_write(cpu) < 0);
	WARN_ON(ret);

	init_completion(&policy->kobj_unregister);
	INIT_WORK(&policy->update, handle_update);

	/* Set governor before ->init, so that driver could check it */
#ifdef CONFIG_HOTPLUG_CPU
	for_each_online_cpu(sibling) {
		struct cpufreq_policy *cp = per_cpu(cpufreq_cpu_data, sibling);
		if (cp && cp->governor &&
		    (cpumask_test_cpu(cpu, cp->related_cpus))) {
			policy->governor = cp->governor;
			found = 1;
			break;
		}
	}
#endif
	if (!found)
		policy->governor = CPUFREQ_DEFAULT_GOVERNOR;
	/* call driver. From then on the cpufreq must be able
	 * to accept all calls to ->verify and ->setpolicy for this CPU
	 */
	ret = cpufreq_driver->init(policy);
	if (ret) {
		pr_debug("initialization failed\n");
		goto err_unlock_policy;
	}

	/*
	 * affected cpus must always be the one, which are online. We aren't
	 * managing offline cpus here.
	 */
	cpumask_and(policy->cpus, policy->cpus, cpu_online_mask);

	policy->user_policy.min = policy->min;
	policy->user_policy.max = policy->max;

	policy->util = 0;

	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
				     CPUFREQ_START, policy);

	ret = cpufreq_add_dev_policy(cpu, policy, dev);
	if (ret) {
		if (ret > 0)
			/* This is a managed cpu, symlink created,
			   exit with 0 */
			ret = 0;
		goto err_unlock_policy;
	}

	ret = cpufreq_add_dev_interface(cpu, policy, dev);
	if (ret)
		goto err_out_unregister;

	unlock_policy_rwsem_write(cpu);

	kobject_uevent(&policy->kobj, KOBJ_ADD);
	module_put(cpufreq_driver->owner);
	pr_debug("initialization complete\n");

	return 0;


err_out_unregister:
	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	for_each_cpu(j, policy->cpus)
		per_cpu(cpufreq_cpu_data, j) = NULL;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	kobject_put(&policy->kobj);
	wait_for_completion(&policy->kobj_unregister);

err_unlock_policy:
	unlock_policy_rwsem_write(cpu);
	free_cpumask_var(policy->related_cpus);
err_free_cpumask:
	free_cpumask_var(policy->cpus);
err_free_policy:
	kfree(policy);
nomem_out:
	module_put(cpufreq_driver->owner);
module_out:
	return ret;
}


/**
 * __cpufreq_remove_dev - remove a CPU device
 *
 * Removes the cpufreq interface for a CPU device.
 * Caller should already have policy_rwsem in write mode for this CPU.
 * This routine frees the rwsem before returning.
 */
static int __cpufreq_remove_dev(struct device *dev, struct subsys_interface *sif)
{
	unsigned int cpu = dev->id;
	unsigned long flags;
	struct cpufreq_policy *data;
	struct kobject *kobj;
	struct completion *cmp;
#ifdef CONFIG_SMP
	struct device *cpu_dev;
	unsigned int j;
#endif

	pr_debug("unregistering CPU %u\n", cpu);

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	data = per_cpu(cpufreq_cpu_data, cpu);

	if (!data) {
		spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
		unlock_policy_rwsem_write(cpu);
		return -EINVAL;
	}
	per_cpu(cpufreq_cpu_data, cpu) = NULL;


#ifdef CONFIG_SMP
	/* if this isn't the CPU which is the parent of the kobj, we
	 * only need to unlink, put and exit
	 */
	if (unlikely(cpu != data->cpu)) {
		pr_debug("removing link\n");
		__cpufreq_governor(data, CPUFREQ_GOV_STOP);
		cpumask_clear_cpu(cpu, data->cpus);
		spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

		__cpufreq_governor(data, CPUFREQ_GOV_START);
		__cpufreq_governor(data, CPUFREQ_GOV_LIMITS);

		kobj = &dev->kobj;
		cpufreq_cpu_put(data);
		unlock_policy_rwsem_write(cpu);
		sysfs_remove_link(kobj, "cpufreq");
		return 0;
	}
#endif

#ifdef CONFIG_SMP

#ifdef CONFIG_HOTPLUG_CPU
	strncpy(per_cpu(cpufreq_policy_save, cpu).gov, data->governor->name,
			CPUFREQ_NAME_LEN);
	per_cpu(cpufreq_policy_save, cpu).min = data->user_policy.min;
	per_cpu(cpufreq_policy_save, cpu).max = data->user_policy.max;
	pr_debug("Saving CPU%d user policy min %d and max %d\n",
			cpu, data->user_policy.min, data->user_policy.max);
#endif

	/* if we have other CPUs still registered, we need to unlink them,
	 * or else wait_for_completion below will lock up. Clean the
	 * per_cpu(cpufreq_cpu_data) while holding the lock, and remove
	 * the sysfs links afterwards.
	 */
	if (unlikely(cpumask_weight(data->cpus) > 1)) {
		for_each_cpu(j, data->cpus) {
			if (j == cpu)
				continue;
			per_cpu(cpufreq_cpu_data, j) = NULL;
		}
	}

	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	if (unlikely(cpumask_weight(data->cpus) > 1)) {
		for_each_cpu(j, data->cpus) {
			if (j == cpu)
				continue;
			pr_debug("removing link for cpu %u\n", j);
#ifdef CONFIG_HOTPLUG_CPU
			strncpy(per_cpu(cpufreq_policy_save, j).gov,
				data->governor->name, CPUFREQ_NAME_LEN);
			per_cpu(cpufreq_policy_save, j).min
						= data->user_policy.min;
			per_cpu(cpufreq_policy_save, j).max
						= data->user_policy.max;
			pr_debug("Saving CPU%d user policy min %d and max %d\n",
					j, data->min, data->max);
#endif
			cpu_dev = get_cpu_device(j);
			kobj = &cpu_dev->kobj;
			unlock_policy_rwsem_write(cpu);
			sysfs_remove_link(kobj, "cpufreq");
			lock_policy_rwsem_write(cpu);
			cpufreq_cpu_put(data);
		}
	}
#else
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
#endif

	if (cpufreq_driver->target)
		__cpufreq_governor(data, CPUFREQ_GOV_STOP);

	kobj = &data->kobj;
	cmp = &data->kobj_unregister;
	unlock_policy_rwsem_write(cpu);
	kobject_put(kobj);

	/* we need to make sure that the underlying kobj is actually
	 * not referenced anymore by anybody before we proceed with
	 * unloading.
	 */
	pr_debug("waiting for dropping of refcount\n");
	wait_for_completion(cmp);
	pr_debug("wait complete\n");

	lock_policy_rwsem_write(cpu);
	if (cpufreq_driver->exit)
		cpufreq_driver->exit(data);
	unlock_policy_rwsem_write(cpu);

#ifdef CONFIG_HOTPLUG_CPU
	/* when the CPU which is the parent of the kobj is hotplugged
	 * offline, check for siblings, and create cpufreq sysfs interface
	 * and symlinks
	 */
	if (unlikely(cpumask_weight(data->cpus) > 1)) {
		/* first sibling now owns the new sysfs dir */
		cpumask_clear_cpu(cpu, data->cpus);
		cpufreq_add_dev(get_cpu_device(cpumask_first(data->cpus)), NULL);

		/* finally remove our own symlink */
		lock_policy_rwsem_write(cpu);
		__cpufreq_remove_dev(dev, sif);
	}
#endif

	free_cpumask_var(data->related_cpus);
	free_cpumask_var(data->cpus);
	kfree(data);

	return 0;
}


static int cpufreq_remove_dev(struct device *dev, struct subsys_interface *sif)
{
	unsigned int cpu = dev->id;
	int retval;

	if (cpu_is_offline(cpu))
		return 0;

	if (unlikely(lock_policy_rwsem_write(cpu)))
		BUG();

	retval = __cpufreq_remove_dev(dev, sif);
	return retval;
}


static void handle_update(struct work_struct *work)
{
	struct cpufreq_policy *policy =
		container_of(work, struct cpufreq_policy, update);
	unsigned int cpu = policy->cpu;
	pr_debug("handle_update for cpu %u called\n", cpu);
	cpufreq_update_policy(cpu);
}

/**
 *	cpufreq_out_of_sync - If actual and saved CPU frequency differs, we're in deep trouble.
 *	@cpu: cpu number
 *	@old_freq: CPU frequency the kernel thinks the CPU runs at
 *	@new_freq: CPU frequency the CPU actually runs at
 *
 *	We adjust to current frequency first, and need to clean up later.
 *	So either call to cpufreq_update_policy() or schedule handle_update()).
 */
static void cpufreq_out_of_sync(unsigned int cpu, unsigned int old_freq,
				unsigned int new_freq)
{
	struct cpufreq_freqs freqs;

	pr_debug("Warning: CPU frequency out of sync: cpufreq and timing "
	       "core thinks of %u, is %u kHz.\n", old_freq, new_freq);

	freqs.cpu = cpu;
	freqs.old = old_freq;
	freqs.new = new_freq;
	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
}

/**
 * cpufreq_quick_get_util - get the CPU utilization from policy->util
 * @cpu: CPU number
 *
 * This is the last known util, without actually getting it from the driver.
 * Return value will be same as what is shown in util in sysfs.
 */
unsigned int cpufreq_quick_get_util(unsigned int cpu)
{
	struct cpufreq_policy *policy = __cpufreq_cpu_get(cpu, 0);
	unsigned int ret_util = 0;

	if (policy) {
		ret_util = policy->util;
		__cpufreq_cpu_put(policy, 0);
	}

	return ret_util;
}
EXPORT_SYMBOL(cpufreq_quick_get_util);

/**
 * cpufreq_quick_get - get the CPU frequency (in kHz) from policy->cur
 * @cpu: CPU number
 *
 * This is the last known freq, without actually getting it from the driver.
 * Return value will be same as what is shown in scaling_cur_freq in sysfs.
 */
unsigned int cpufreq_quick_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	unsigned int ret_freq = 0;

	if (policy) {
		ret_freq = policy->cur;
		cpufreq_cpu_put(policy);
	}

	return ret_freq;
}
EXPORT_SYMBOL(cpufreq_quick_get);

/**
 * cpufreq_quick_get_max - get the max reported CPU frequency for this CPU
 * @cpu: CPU number
 *
 * Just return the max possible frequency for a given CPU.
 */
unsigned int cpufreq_quick_get_max(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	unsigned int ret_freq = 0;

	if (policy) {
		ret_freq = policy->max;
		cpufreq_cpu_put(policy);
	}

	return ret_freq;
}
EXPORT_SYMBOL(cpufreq_quick_get_max);


static unsigned int __cpufreq_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = per_cpu(cpufreq_cpu_data, cpu);
	unsigned int ret_freq = 0;

	if (!cpufreq_driver->get)
		return ret_freq;

	ret_freq = cpufreq_driver->get(cpu);

	if (ret_freq && policy->cur &&
		!(cpufreq_driver->flags & CPUFREQ_CONST_LOOPS)) {
		/* verify no discrepancy between actual and
					saved value exists */
		if (unlikely(ret_freq != policy->cur)) {
			cpufreq_out_of_sync(cpu, policy->cur, ret_freq);
			schedule_work(&policy->update);
		}
	}

	return ret_freq;
}

/**
 * cpufreq_get - get the current CPU frequency (in kHz)
 * @cpu: CPU number
 *
 * Get the CPU current (static) CPU frequency
 */
unsigned int cpufreq_get(unsigned int cpu)
{
	unsigned int ret_freq = 0;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

	if (!policy)
		goto out;

	if (unlikely(lock_policy_rwsem_read(cpu)))
		goto out_policy;

	ret_freq = __cpufreq_get(cpu);

	unlock_policy_rwsem_read(cpu);

out_policy:
	cpufreq_cpu_put(policy);
out:
	return ret_freq;
}
EXPORT_SYMBOL(cpufreq_get);

static struct subsys_interface cpufreq_interface = {
	.name		= "cpufreq",
	.subsys		= &cpu_subsys,
	.add_dev	= cpufreq_add_dev,
	.remove_dev	= cpufreq_remove_dev,
};


/**
 * cpufreq_bp_suspend - Prepare the boot CPU for system suspend.
 *
 * This function is only executed for the boot processor.  The other CPUs
 * have been put offline by means of CPU hotplug.
 */
static int cpufreq_bp_suspend(void)
{
	int ret = 0;

	int cpu = smp_processor_id();
	struct cpufreq_policy *cpu_policy;

	pr_debug("suspending cpu %u\n", cpu);

	/* If there's no policy for the boot CPU, we have nothing to do. */
	cpu_policy = cpufreq_cpu_get(cpu);
	if (!cpu_policy)
		return 0;

	if (cpufreq_driver->suspend) {
		ret = cpufreq_driver->suspend(cpu_policy);
		if (ret)
			printk(KERN_ERR "cpufreq: suspend failed in ->suspend "
					"step on CPU %u\n", cpu_policy->cpu);
	}

	cpufreq_cpu_put(cpu_policy);
	return ret;
}

/**
 * cpufreq_bp_resume - Restore proper frequency handling of the boot CPU.
 *
 *	1.) resume CPUfreq hardware support (cpufreq_driver->resume())
 *	2.) schedule call cpufreq_update_policy() ASAP as interrupts are
 *	    restored. It will verify that the current freq is in sync with
 *	    what we believe it to be. This is a bit later than when it
 *	    should be, but nonethteless it's better than calling
 *	    cpufreq_driver->get() here which might re-enable interrupts...
 *
 * This function is only executed for the boot CPU.  The other CPUs have not
 * been turned on yet.
 */
static void cpufreq_bp_resume(void)
{
	int ret = 0;

	int cpu = smp_processor_id();
	struct cpufreq_policy *cpu_policy;

	pr_debug("resuming cpu %u\n", cpu);

	/* If there's no policy for the boot CPU, we have nothing to do. */
	cpu_policy = cpufreq_cpu_get(cpu);
	if (!cpu_policy)
		return;

	if (cpufreq_driver->resume) {
		ret = cpufreq_driver->resume(cpu_policy);
		if (ret) {
			printk(KERN_ERR "cpufreq: resume failed in ->resume "
					"step on CPU %u\n", cpu_policy->cpu);
			goto fail;
		}
	}

	schedule_work(&cpu_policy->update);

fail:
	cpufreq_cpu_put(cpu_policy);
}

static struct syscore_ops cpufreq_syscore_ops = {
	.suspend	= cpufreq_bp_suspend,
	.resume		= cpufreq_bp_resume,
};


/*********************************************************************
 *                     NOTIFIER LISTS INTERFACE                      *
 *********************************************************************/

/**
 *	cpufreq_register_notifier - register a driver with cpufreq
 *	@nb: notifier function to register
 *      @list: CPUFREQ_TRANSITION_NOTIFIER or CPUFREQ_POLICY_NOTIFIER
 *
 *	Add a driver to one of two lists: either a list of drivers that
 *      are notified about clock rate changes (once before and once after
 *      the transition), or a list of drivers that are notified about
 *      changes in cpufreq policy.
 *
 *	This function may sleep, and has the same return conditions as
 *	blocking_notifier_chain_register.
 */
int cpufreq_register_notifier(struct notifier_block *nb, unsigned int list)
{
	int ret;

	WARN_ON(!init_cpufreq_transition_notifier_list_called);

	switch (list) {
	case CPUFREQ_TRANSITION_NOTIFIER:
		ret = srcu_notifier_chain_register(
				&cpufreq_transition_notifier_list, nb);
		break;
	case CPUFREQ_POLICY_NOTIFIER:
		ret = blocking_notifier_chain_register(
				&cpufreq_policy_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(cpufreq_register_notifier);


/**
 *	cpufreq_unregister_notifier - unregister a driver with cpufreq
 *	@nb: notifier block to be unregistered
 *      @list: CPUFREQ_TRANSITION_NOTIFIER or CPUFREQ_POLICY_NOTIFIER
 *
 *	Remove a driver from the CPU frequency notifier list.
 *
 *	This function may sleep, and has the same return conditions as
 *	blocking_notifier_chain_unregister.
 */
int cpufreq_unregister_notifier(struct notifier_block *nb, unsigned int list)
{
	int ret;

	switch (list) {
	case CPUFREQ_TRANSITION_NOTIFIER:
		ret = srcu_notifier_chain_unregister(
				&cpufreq_transition_notifier_list, nb);
		break;
	case CPUFREQ_POLICY_NOTIFIER:
		ret = blocking_notifier_chain_unregister(
				&cpufreq_policy_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(cpufreq_unregister_notifier);

#if defined(CONFIG_LGE_LOW_BATT_LIMIT)
#define BOOT_ARGS "chosen"
static long	soc = 0;
#include <linux/of.h>

static int parse_batt_soc_bootarg(void)
{
	struct device_node *chosen_node;
	static const char *cmd_line;
	int rc = 0, len = 0, name_len = 0, cmd_len = 0;
	char batt_soc[3] = {0,};
	char *sidx, *eidx;
	chosen_node = of_find_node_by_name(NULL, BOOT_ARGS);
	if (!chosen_node) {
		pr_err("%s: get chosen node failed\n", __func__);
		return -ENODEV;
	}

	cmd_line = of_get_property(chosen_node, "bootargs", &len);
	if (!cmd_line || len <= 0) {
		pr_err("%s: get bootargs failed\n", __func__);
		return -ENODEV;
	}

	name_len = strlen("batt.soc=");
	cmd_len = strlen(cmd_line);
	sidx = strnstr(cmd_line, "batt.soc=", cmd_len);
	if (!sidx) {
		pr_err("failed batt soc from boot command\n");
		return -ENODEV;
	}
	sidx += name_len;

	eidx = strnstr(sidx, " ", 10);

	if (!eidx) {
		eidx = sidx + strlen(sidx) + 1;
	}

	if (eidx <= sidx) {
		return -ENODEV;
	}

	*eidx = 0;
	len = eidx - sidx + 1;
	if (len <= 0) {
		return -ENODEV;
	}

	strncpy(batt_soc, sidx, strlen(sidx));
	of_node_put(chosen_node);
	if (strict_strtol(batt_soc, 10, &soc) != 0) {
		return -ENODEV;
	}

	return rc;
}

#define MAX_CPUS (4)
#define LOW_BATT_LIMIT_THRESHOLD (5)
#define PREV_FREQ_INDEX			(2)
typedef struct low_battery_llimit {
	struct cpufreq_frequency_table *table;
	int	last_cpufreq_index;
}low_batt_limitation;
static  low_batt_limitation low_battery_limit[MAX_CPUS];
static int out_low_battery_limit = 0;
static int set_clear_limit(const char *val, struct kernel_param *kp)
{
	int ret = 0;
	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}
	out_low_battery_limit = 1;
	pr_info(" low batt limitation is clear by thermal\n");
	return ret;
}

module_param_call(out_low_battery_limit, set_clear_limit,
	param_get_int, &out_low_battery_limit, 0644);

static void init_freq_table(void)
{
	int cpu_i , freq_i;
	for( cpu_i = 0 ; cpu_i < MAX_CPUS; cpu_i++) {
		low_battery_limit[cpu_i].table = 0;
		low_battery_limit[cpu_i].last_cpufreq_index = 0;

		low_battery_limit[cpu_i].table = cpufreq_frequency_get_table(cpu_i);
		if(low_battery_limit[cpu_i].table > 0) {
			for (freq_i = 0; (low_battery_limit[cpu_i].table[freq_i].frequency != CPUFREQ_TABLE_END); freq_i++) {
				low_battery_limit[cpu_i].last_cpufreq_index = freq_i;
				if (low_battery_limit[cpu_i].table[freq_i].frequency == CPUFREQ_ENTRY_INVALID) {
					continue;
				}
			}
		}
	}
}
#endif
/*********************************************************************
 *                              GOVERNORS                            *
 *********************************************************************/

#if defined(CONFIG_LGE_LOW_BATT_LIMIT)
#if defined(CONFIG_MACH_MSM8974_G3_GLOBAL_COM) || defined(CONFIG_MACH_MSM8974_G3_TMO_US)
static unsigned int old_max_freq = 0;
static unsigned int restore_flag = 1;
#endif
#endif
int __cpufreq_driver_target(struct cpufreq_policy *policy,
			    unsigned int target_freq,
			    unsigned int relation)
{
	int retval = -EINVAL;
#if defined(CONFIG_LGE_LOW_BATT_LIMIT)
	int update_index = 0;
#endif
	if (cpufreq_disabled())
		return -ENODEV;
#if defined(CONFIG_LGE_LOW_BATT_LIMIT)
	if(!low_battery_limit[policy->cpu].table) {
		init_freq_table();
	}
#endif
	pr_debug("target for CPU %u: %u kHz, relation %u \n", policy->cpu,
		target_freq, relation );

	if (target_freq == policy->cur)
		return 0;

#if defined(CONFIG_LGE_LOW_BATT_LIMIT)
#if defined(CONFIG_MACH_MSM8974_G3_GLOBAL_COM) || defined(CONFIG_MACH_MSM8974_G3_TMO_US)
	if (old_max_freq == 0)
		old_max_freq = policy->max;
	if (!out_low_battery_limit) {
		/* limit to previous freq. */
		update_index = (low_battery_limit[policy->cpu].last_cpufreq_index) - PREV_FREQ_INDEX;
		if (low_battery_limit[policy->cpu].table > 0 && update_index >= 0) {
			/* adjust max freq to target freq */
			policy->max = low_battery_limit[policy->cpu].table[--update_index].frequency;
			if(target_freq > policy->max)
				target_freq = policy->max;
		} else {
			pr_info("low_limit_table is still NULL== %u\n",target_freq);
		}
	} else if (restore_flag == 1 && out_low_battery_limit == 1) {
		policy->max = old_max_freq;
		restore_flag = 0;
	}
#else
	if (policy->max == target_freq && soc <= LOW_BATT_LIMIT_THRESHOLD
		&& !out_low_battery_limit) {
		// limit to previous freq.
		update_index = (low_battery_limit[policy->cpu].last_cpufreq_index) - PREV_FREQ_INDEX;
		if (low_battery_limit[policy->cpu].table > 0 &&
			update_index >= 0) {
			target_freq = low_battery_limit[policy->cpu].table[--update_index].frequency;
		} else {
			pr_info("low_limit_table is still NULL== %u\n",target_freq);
		}
		pr_info("target for CPU %u: %u kHz, soc %ld\n", policy->cpu, target_freq, soc);
	}
#endif
#endif
	if (cpu_online(policy->cpu) && cpufreq_driver->target)
		retval = cpufreq_driver->target(policy, target_freq, relation);

	return retval;
}
EXPORT_SYMBOL_GPL(__cpufreq_driver_target);

int cpufreq_driver_target(struct cpufreq_policy *policy,
			  unsigned int target_freq,
			  unsigned int relation)
{
	int ret = -EINVAL;

	policy = cpufreq_cpu_get(policy->cpu);
	if (!policy)
		goto no_policy;

	if (unlikely(lock_policy_rwsem_write(policy->cpu)))
		goto fail;

	ret = __cpufreq_driver_target(policy, target_freq, relation);

	unlock_policy_rwsem_write(policy->cpu);

fail:
	cpufreq_cpu_put(policy);
no_policy:
	return ret;
}
EXPORT_SYMBOL_GPL(cpufreq_driver_target);

int __cpufreq_driver_getavg(struct cpufreq_policy *policy, unsigned int cpu)
{
	int ret = 0;

	if (!(cpu_online(cpu) && cpufreq_driver->getavg))
		return 0;

	policy = cpufreq_cpu_get(policy->cpu);
	if (!policy)
		return -EINVAL;

	ret = cpufreq_driver->getavg(policy, cpu);

	cpufreq_cpu_put(policy);
	return ret;
}
EXPORT_SYMBOL_GPL(__cpufreq_driver_getavg);

/*
 * when "event" is CPUFREQ_GOV_LIMITS
 */

static int __cpufreq_governor(struct cpufreq_policy *policy,
					unsigned int event)
{
	int ret;

	/* Only must be defined when default governor is known to have latency
	   restrictions, like e.g. conservative or ondemand.
	   That this is the case is already ensured in Kconfig
	*/
#ifdef CONFIG_CPU_FREQ_GOV_PERFORMANCE
	struct cpufreq_governor *gov = &cpufreq_gov_performance;
#else
	struct cpufreq_governor *gov = NULL;
#endif

	if (policy->governor->max_transition_latency &&
	    policy->cpuinfo.transition_latency >
	    policy->governor->max_transition_latency) {
		if (!gov)
			return -EINVAL;
		else {
			printk(KERN_WARNING "%s governor failed, too long"
			       " transition latency of HW, fallback"
			       " to %s governor\n",
			       policy->governor->name,
			       gov->name);
			policy->governor = gov;
		}
	}

	if (!try_module_get(policy->governor->owner))
		return -EINVAL;

	pr_debug("__cpufreq_governor for CPU %u, event %u\n",
						policy->cpu, event);
	ret = policy->governor->governor(policy, event);

	/* we keep one module reference alive for
			each CPU governed by this CPU */
	if ((event != CPUFREQ_GOV_START) || ret)
		module_put(policy->governor->owner);
	if ((event == CPUFREQ_GOV_STOP) && !ret)
		module_put(policy->governor->owner);

	return ret;
}


int cpufreq_register_governor(struct cpufreq_governor *governor)
{
	int err;

	if (!governor)
		return -EINVAL;

	if (cpufreq_disabled())
		return -ENODEV;

	mutex_lock(&cpufreq_governor_mutex);

	err = -EBUSY;
	if (__find_governor(governor->name) == NULL) {
		err = 0;
		list_add(&governor->governor_list, &cpufreq_governor_list);
	}

	mutex_unlock(&cpufreq_governor_mutex);
	return err;
}
EXPORT_SYMBOL_GPL(cpufreq_register_governor);


void cpufreq_unregister_governor(struct cpufreq_governor *governor)
{
#ifdef CONFIG_HOTPLUG_CPU
	int cpu;
#endif

	if (!governor)
		return;

	if (cpufreq_disabled())
		return;

#ifdef CONFIG_HOTPLUG_CPU
	for_each_present_cpu(cpu) {
		if (cpu_online(cpu))
			continue;
		if (!strcmp(per_cpu(cpufreq_policy_save, cpu).gov,
					governor->name))
			strcpy(per_cpu(cpufreq_policy_save, cpu).gov, "\0");
		per_cpu(cpufreq_policy_save, cpu).min = 0;
		per_cpu(cpufreq_policy_save, cpu).max = 0;
	}
#endif

	mutex_lock(&cpufreq_governor_mutex);
	list_del(&governor->governor_list);
	mutex_unlock(&cpufreq_governor_mutex);
	return;
}
EXPORT_SYMBOL_GPL(cpufreq_unregister_governor);



/*********************************************************************
 *                          POLICY INTERFACE                         *
 *********************************************************************/

/**
 * cpufreq_get_policy - get the current cpufreq_policy
 * @policy: struct cpufreq_policy into which the current cpufreq_policy
 *	is written
 *
 * Reads the current cpufreq policy.
 */
int cpufreq_get_policy(struct cpufreq_policy *policy, unsigned int cpu)
{
	struct cpufreq_policy *cpu_policy;
	if (!policy)
		return -EINVAL;

	cpu_policy = cpufreq_cpu_get(cpu);
	if (!cpu_policy)
		return -EINVAL;

	memcpy(policy, cpu_policy, sizeof(struct cpufreq_policy));

	cpufreq_cpu_put(cpu_policy);
	return 0;
}
EXPORT_SYMBOL(cpufreq_get_policy);

/*
 * data   : current policy.
 * policy : policy to be set.
 */
static int __cpufreq_set_policy(struct cpufreq_policy *data,
				struct cpufreq_policy *policy)
{
	int ret = 0;
#ifdef CONFIG_UNI_CPU_POLICY_LIMIT
	struct cpufreq_policy *cpu0_policy = NULL;
#endif
	unsigned int qmin, qmax;
	unsigned int pmin = policy->min;
	unsigned int pmax = policy->max;

	qmin = min((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MIN),
		   data->user_policy.max);
	qmax = max((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MAX),
		   data->user_policy.min);

	pr_debug("setting new policy for CPU %u: %u - %u (%u - %u) kHz\n",
		policy->cpu, pmin, pmax, qmin, qmax);

	/* clamp the new policy to PM QoS limits */
	policy->min = max(pmin, qmin);
	policy->max = min(pmax, qmax);

	memcpy(&policy->cpuinfo, &data->cpuinfo,
				sizeof(struct cpufreq_cpuinfo));

	if (policy->min > data->user_policy.max ||
	    policy->max < data->user_policy.min) {
		ret = -EINVAL;
		goto error_out;
	}

	/* verify the cpu speed can be set within this limit */
	ret = cpufreq_driver->verify(policy);
	if (ret)
		goto error_out;

	/* adjust if necessary - all reasons */
	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
			CPUFREQ_ADJUST, policy);

	/* adjust if necessary - hardware incompatibility*/
	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
			CPUFREQ_INCOMPATIBLE, policy);

	/* verify the cpu speed can be set within this limit,
	   which might be different to the first one */
	ret = cpufreq_driver->verify(policy);
	if (ret)
		goto error_out;

	/* notification of the new policy */
	blocking_notifier_call_chain(&cpufreq_policy_notifier_list,
			CPUFREQ_NOTIFY, policy);

#ifdef CONFIG_UNI_CPU_POLICY_LIMIT
	if (policy->cpu) {
		cpu0_policy = __cpufreq_cpu_get(0, 0);
		data->min = cpu0_policy->min;
		data->max = cpu0_policy->max;
	} else {
		data->min = policy->min;
		data->max = policy->max;
	}
#else
	data->min = policy->min;
	data->max = policy->max;
#endif

	pr_debug("new min and max freqs are %u - %u kHz\n",
					data->min, data->max);

	if (cpufreq_driver->setpolicy) {
		data->policy = policy->policy;
		pr_debug("setting range\n");
		ret = cpufreq_driver->setpolicy(policy);
	} else {
		if (policy->governor != data->governor) {
			/* save old, working values */
			struct cpufreq_governor *old_gov = data->governor;

			pr_debug("governor switch\n");

			/* end old governor */
			if (data->governor)
				__cpufreq_governor(data, CPUFREQ_GOV_STOP);

			/* start new governor */
#ifdef CONFIG_UNI_CPU_POLICY_LIMIT
			if (policy->cpu && cpu0_policy) {
				data->governor = cpu0_policy->governor;
			} else {
				data->governor = policy->governor;
			}
#endif
			data->governor = policy->governor;
#endif

			if (__cpufreq_governor(data, CPUFREQ_GOV_START)) {
				/* new governor failed, so re-start old one */
				pr_debug("starting governor %s failed\n",
							data->governor->name);
				if (old_gov) {
					data->governor = old_gov;
					__cpufreq_governor(data,
							   CPUFREQ_GOV_START);
				}
				ret = -EINVAL;
				goto error_out;
			}
			/* might be a policy change, too, so fall through */
		}
		pr_debug("governor: change or update limits\n");
		__cpufreq_governor(data, CPUFREQ_GOV_LIMITS);
	}

error_out:
#ifdef CONFIG_UNI_CPU_POLICY_LIMIT
	if (cpu0_policy) {
		__cpufreq_cpu_put(cpu0_policy, 0);
	}
#endif
	/* restore the limits that the policy requested */
	policy->min = pmin;
	policy->max = pmax;
	return ret;
}

/**
 *	cpufreq_update_policy - re-evaluate an existing cpufreq policy
 *	@cpu: CPU which shall be re-evaluated
 *
 *	Useful for policy notifiers which have different necessities
 *	at different times.
 */
int cpufreq_update_policy(unsigned int cpu)
{
	struct cpufreq_policy *data = cpufreq_cpu_get(cpu);
	struct cpufreq_policy policy;
	int ret;

	if (!data) {
		ret = -ENODEV;
		goto no_policy;
	}

	if (unlikely(lock_policy_rwsem_write(cpu))) {
		ret = -EINVAL;
		goto fail;
	}

	pr_debug("updating policy for CPU %u\n", cpu);
	memcpy(&policy, data, sizeof(struct cpufreq_policy));
	policy.min = data->user_policy.min;
	policy.max = data->user_policy.max;
	policy.policy = data->user_policy.policy;
	policy.governor = data->user_policy.governor;

	/* BIOS might change freq behind our back
	  -> ask driver for current freq and notify governors about a change */
	if (cpufreq_driver->get) {
		policy.cur = cpufreq_driver->get(cpu);
		if (!data->cur) {
			pr_debug("Driver did not initialize current freq");
			data->cur = policy.cur;
		} else {
			if (data->cur != policy.cur)
				cpufreq_out_of_sync(cpu, data->cur,
								policy.cur);
		}
	}

	ret = __cpufreq_set_policy(data, &policy);

	unlock_policy_rwsem_write(cpu);

fail:
	cpufreq_cpu_put(data);
no_policy:
	return ret;
}
EXPORT_SYMBOL(cpufreq_update_policy);

/*
 *	cpufreq_set_gov - set governor for a cpu
 *	@cpu: CPU whose governor needs to be changed
 *	@target_gov: new governor to be set
 */
int cpufreq_set_gov(char *target_gov, unsigned int cpu)
{
	int ret = 0;
	struct cpufreq_policy new_policy;
	struct cpufreq_policy *cur_policy;

	if (target_gov == NULL)
		return -EINVAL;

	/* Get current governer */
	cur_policy = cpufreq_cpu_get(cpu);
	if (!cur_policy)
		return -EINVAL;

	if (lock_policy_rwsem_read(cur_policy->cpu) < 0) {
		ret = -EINVAL;
		goto err_out;
	}

	if (cur_policy->governor)
		ret = strncmp(cur_policy->governor->name, target_gov,
					strlen(target_gov));
	else {
		unlock_policy_rwsem_read(cur_policy->cpu);
		ret = -EINVAL;
		goto err_out;
	}
	unlock_policy_rwsem_read(cur_policy->cpu);

	if (!ret) {
		pr_debug(" Target governer & current governer is same\n");
		ret = -EINVAL;
		goto err_out;
	} else {
		new_policy = *cur_policy;
		if (cpufreq_parse_governor(target_gov, &new_policy.policy,
				&new_policy.governor)) {
			ret = -EINVAL;
			goto err_out;
		}

		if (lock_policy_rwsem_write(cur_policy->cpu) < 0) {
			ret = -EINVAL;
			goto err_out;
		}

		ret = __cpufreq_set_policy(cur_policy, &new_policy);

		cur_policy->user_policy.policy = cur_policy->policy;
		cur_policy->user_policy.governor = cur_policy->governor;

		unlock_policy_rwsem_write(cur_policy->cpu);
	}
err_out:
	cpufreq_cpu_put(cur_policy);
	return ret;
}
EXPORT_SYMBOL(cpufreq_set_gov);

static int __cpuinit cpufreq_cpu_callback(struct notifier_block *nfb,
					unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	struct device *dev;

	dev = get_cpu_device(cpu);
	if (dev) {
		switch (action) {
		case CPU_ONLINE:
		case CPU_ONLINE_FROZEN:
			cpufreq_add_dev(dev, NULL);
			break;
		case CPU_DOWN_PREPARE:
		case CPU_DOWN_PREPARE_FROZEN:
			if (unlikely(lock_policy_rwsem_write(cpu)))
				BUG();

			__cpufreq_remove_dev(dev, NULL);
			break;
		case CPU_DOWN_FAILED:
		case CPU_DOWN_FAILED_FROZEN:
			cpufreq_add_dev(dev, NULL);
			break;
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block __refdata cpufreq_cpu_notifier = {
    .notifier_call = cpufreq_cpu_callback,
};

/*********************************************************************
 *               REGISTER / UNREGISTER CPUFREQ DRIVER                *
 *********************************************************************/

/**
 * cpufreq_register_driver - register a CPU Frequency driver
 * @driver_data: A struct cpufreq_driver containing the values#
 * submitted by the CPU Frequency driver.
 *
 *   Registers a CPU Frequency driver to this core code. This code
 * returns zero on success, -EBUSY when another driver got here first
 * (and isn't unregistered in the meantime).
 *
 */
int cpufreq_register_driver(struct cpufreq_driver *driver_data)
{
	unsigned long flags;
	int ret;

	if (cpufreq_disabled())
		return -ENODEV;

	if (!driver_data || !driver_data->verify || !driver_data->init ||
	    ((!driver_data->setpolicy) && (!driver_data->target)))
		return -EINVAL;

	pr_debug("trying to register driver %s\n", driver_data->name);

	if (driver_data->setpolicy)
		driver_data->flags |= CPUFREQ_CONST_LOOPS;

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	if (cpufreq_driver) {
		spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
		return -EBUSY;
	}
	cpufreq_driver = driver_data;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	ret = subsys_interface_register(&cpufreq_interface);
	if (ret)
		goto err_null_driver;

	if (!(cpufreq_driver->flags & CPUFREQ_STICKY)) {
		int i;
		ret = -ENODEV;

		/* check for at least one working CPU */
		for (i = 0; i < nr_cpu_ids; i++)
			if (cpu_possible(i) && per_cpu(cpufreq_cpu_data, i)) {
				ret = 0;
				break;
			}

		/* if all ->init() calls failed, unregister */
		if (ret) {
			pr_debug("no CPU initialized for driver %s\n",
							driver_data->name);
			goto err_if_unreg;
		}
	}

	register_hotcpu_notifier(&cpufreq_cpu_notifier);
	pr_debug("driver %s up and running\n", driver_data->name);

	return 0;
err_if_unreg:
	subsys_interface_unregister(&cpufreq_interface);
err_null_driver:
	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	cpufreq_driver = NULL;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(cpufreq_register_driver);


/**
 * cpufreq_unregister_driver - unregister the current CPUFreq driver
 *
 *    Unregister the current CPUFreq driver. Only call this if you have
 * the right to do so, i.e. if you have succeeded in initialising before!
 * Returns zero if successful, and -EINVAL if the cpufreq_driver is
 * currently not initialised.
 */
int cpufreq_unregister_driver(struct cpufreq_driver *driver)
{
	unsigned long flags;

	if (!cpufreq_driver || (driver != cpufreq_driver))
		return -EINVAL;

	pr_debug("unregistering driver %s\n", driver->name);

	subsys_interface_unregister(&cpufreq_interface);
	unregister_hotcpu_notifier(&cpufreq_cpu_notifier);

	spin_lock_irqsave(&cpufreq_driver_lock, flags);
	cpufreq_driver = NULL;
	spin_unlock_irqrestore(&cpufreq_driver_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(cpufreq_unregister_driver);

static int cpu_freq_notify(struct notifier_block *b,
			   unsigned long l, void *v);

static struct notifier_block min_freq_notifier = {
	.notifier_call = cpu_freq_notify,
};
static struct notifier_block max_freq_notifier = {
	.notifier_call = cpu_freq_notify,
};

static int cpu_freq_notify(struct notifier_block *b,
			   unsigned long l, void *v)
{
	int cpu;
	pr_debug("PM QoS %s %lu\n",
		b == &min_freq_notifier ? "min" : "max", l);
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		if (policy) {
			cpufreq_update_policy(policy->cpu);
			cpufreq_cpu_put(policy);
		}
	}
	return NOTIFY_OK;
}

static int __init cpufreq_core_init(void)
{
	int cpu;
	int rc;

	if (cpufreq_disabled())
		return -ENODEV;

	for_each_possible_cpu(cpu) {
		per_cpu(cpufreq_policy_cpu, cpu) = -1;
		init_rwsem(&per_cpu(cpu_policy_rwsem, cpu));
	}

	cpufreq_global_kobject = kobject_create_and_add("cpufreq", &cpu_subsys.dev_root->kobj);
	BUG_ON(!cpufreq_global_kobject);
#if defined(CONFIG_LGE_LOW_BATT_LIMIT)
	parse_batt_soc_bootarg();
#endif
	register_syscore_ops(&cpufreq_syscore_ops);
	rc = pm_qos_add_notifier(PM_QOS_CPU_FREQ_MIN,
				 &min_freq_notifier);
	BUG_ON(rc);
	rc = pm_qos_add_notifier(PM_QOS_CPU_FREQ_MAX,
				 &max_freq_notifier);
	BUG_ON(rc);

#ifdef CONFIG_MULTI_CPU_POLICY_LIMIT
	rc = sysfs_create_group(cpufreq_global_kobject, &all_cpus_attr_group);
#endif	/* CONFIG_MULTI_CPU_POLICY_LIMIT */

	return 0;
}
core_initcall(cpufreq_core_init);
