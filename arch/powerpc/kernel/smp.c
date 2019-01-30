/*
 * SMP support for ppc.
 *
 * Written by Cort Dougan (cort@cs.nmt.edu) borrowing a great
 * deal of code from the sparc and intel versions.
 *
 * Copyright (C) 1999 Cort Dougan <cort@cs.nmt.edu>
 *
 * PowerPC-64 Support added by Dave Engebretsen, Peter Bergner, and
 * Mike Corrigan {engebret|bergner|mikec}@us.ibm.com
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#undef DEBUG

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/cache.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/topology.h>

#include <asm/ptrace.h>
#include <linux/atomic.h>
#include <asm/irq.h>
#include <asm/hw_irq.h>
#include <asm/kvm_ppc.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/prom.h>
#include <asm/smp.h>
#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/cputhreads.h>
#include <asm/cputable.h>
#include <asm/mpic.h>
#include <asm/vdso_datapage.h>
#ifdef CONFIG_PPC64
#include <asm/paca.h>
#endif
#include <asm/vdso.h>
#include <asm/debug.h>
#include <asm/kexec.h>

#ifdef DEBUG
#include <asm/udbg.h>
#define DBG(fmt...) udbg_printf(fmt)
#else
#define DBG(fmt...)
#endif

#ifdef CONFIG_HOTPLUG_CPU
/* State of each CPU during hotplug phases */
static DEFINE_PER_CPU(int, cpu_state) = { 0 };
#endif

struct thread_info *secondary_ti;
bool has_big_cores;

DEFINE_PER_CPU(cpumask_var_t, cpu_sibling_map);
DEFINE_PER_CPU(cpumask_var_t, cpu_smallcore_map);
DEFINE_PER_CPU(cpumask_var_t, cpu_l2_cache_map);
DEFINE_PER_CPU(cpumask_var_t, cpu_core_map);

EXPORT_PER_CPU_SYMBOL(cpu_sibling_map);
EXPORT_PER_CPU_SYMBOL(cpu_l2_cache_map);
EXPORT_PER_CPU_SYMBOL(cpu_core_map);
EXPORT_SYMBOL_GPL(has_big_cores);

#define MAX_THREAD_LIST_SIZE	8
#define THREAD_GROUP_SHARE_L1   1
struct thread_groups {
	unsigned int property;
	unsigned int nr_groups;
	unsigned int threads_per_group;
	unsigned int thread_list[MAX_THREAD_LIST_SIZE];
};

/*
 * On big-cores system, cpu_l1_cache_map for each CPU corresponds to
 * the set its siblings that share the L1-cache.
 */
DEFINE_PER_CPU(cpumask_var_t, cpu_l1_cache_map);

/* SMP operations for this machine */
struct smp_ops_t *smp_ops;

/* Can't be static due to PowerMac hackery */
volatile unsigned int cpu_callin_map[NR_CPUS];

int smt_enabled_at_boot = 1;

static void (*crash_ipi_function_ptr)(struct pt_regs *) = NULL;

/*
 * Returns 1 if the specified cpu should be brought up during boot.
 * Used to inhibit booting threads if they've been disabled or
 * limited on the command line
 */
int smp_generic_cpu_bootable(unsigned int nr)
{
	/* Special case - we inhibit secondary thread startup
	 * during boot if the user requests it.
	 */
	if (system_state == SYSTEM_BOOTING && cpu_has_feature(CPU_FTR_SMT)) {
		if (!smt_enabled_at_boot && cpu_thread_in_core(nr) != 0)
			return 0;
		if (smt_enabled_at_boot
		    && cpu_thread_in_core(nr) >= smt_enabled_at_boot)
			return 0;
	}

	return 1;
}


#ifdef CONFIG_PPC64
int smp_generic_kick_cpu(int nr)
{
	BUG_ON(nr < 0 || nr >= NR_CPUS);

	/*
	 * The processor is currently spinning, waiting for the
	 * cpu_start field to become non-zero After we set cpu_start,
	 * the processor will continue on to secondary_start
	 */
	if (!paca[nr].cpu_start) {
		paca[nr].cpu_start = 1;
		smp_mb();
		return 0;
	}

#ifdef CONFIG_HOTPLUG_CPU
	/*
	 * Ok it's not there, so it might be soft-unplugged, let's
	 * try to bring it back
	 */
	generic_set_cpu_up(nr);
	smp_wmb();
	smp_send_reschedule(nr);
#endif /* CONFIG_HOTPLUG_CPU */

	return 0;
}
#endif /* CONFIG_PPC64 */

static irqreturn_t call_function_action(int irq, void *data)
{
	generic_smp_call_function_interrupt();
	return IRQ_HANDLED;
}

static irqreturn_t reschedule_action(int irq, void *data)
{
	scheduler_ipi();
	return IRQ_HANDLED;
}

static irqreturn_t tick_broadcast_ipi_action(int irq, void *data)
{
	tick_broadcast_ipi_handler();
	return IRQ_HANDLED;
}

static irqreturn_t debug_ipi_action(int irq, void *data)
{
	if (crash_ipi_function_ptr) {
		crash_ipi_function_ptr(get_irq_regs());
		return IRQ_HANDLED;
	}

#ifdef CONFIG_DEBUGGER
	debugger_ipi(get_irq_regs());
#endif /* CONFIG_DEBUGGER */

	return IRQ_HANDLED;
}

static irq_handler_t smp_ipi_action[] = {
	[PPC_MSG_CALL_FUNCTION] =  call_function_action,
	[PPC_MSG_RESCHEDULE] = reschedule_action,
	[PPC_MSG_TICK_BROADCAST] = tick_broadcast_ipi_action,
	[PPC_MSG_DEBUGGER_BREAK] = debug_ipi_action,
};

const char *smp_ipi_name[] = {
	[PPC_MSG_CALL_FUNCTION] =  "ipi call function",
	[PPC_MSG_RESCHEDULE] = "ipi reschedule",
	[PPC_MSG_TICK_BROADCAST] = "ipi tick-broadcast",
	[PPC_MSG_DEBUGGER_BREAK] = "ipi debugger",
};

/* optional function to request ipi, for controllers with >= 4 ipis */
int smp_request_message_ipi(int virq, int msg)
{
	int err;

	if (msg < 0 || msg > PPC_MSG_DEBUGGER_BREAK) {
		return -EINVAL;
	}
#if !defined(CONFIG_DEBUGGER) && !defined(CONFIG_KEXEC)
	if (msg == PPC_MSG_DEBUGGER_BREAK) {
		return 1;
	}
#endif
	err = request_irq(virq, smp_ipi_action[msg],
			  IRQF_PERCPU | IRQF_NO_THREAD | IRQF_NO_SUSPEND,
			  smp_ipi_name[msg], NULL);
	WARN(err < 0, "unable to request_irq %d for %s (rc %d)\n",
		virq, smp_ipi_name[msg], err);

	return err;
}

#ifdef CONFIG_PPC_SMP_MUXED_IPI
struct cpu_messages {
	int messages;			/* current messages */
	unsigned long data;		/* data for cause ipi */
};
static DEFINE_PER_CPU_SHARED_ALIGNED(struct cpu_messages, ipi_message);

void smp_muxed_ipi_set_data(int cpu, unsigned long data)
{
	struct cpu_messages *info = &per_cpu(ipi_message, cpu);

	info->data = data;
}

void smp_muxed_ipi_message_pass(int cpu, int msg)
{
	struct cpu_messages *info = &per_cpu(ipi_message, cpu);
	char *message = (char *)&info->messages;

	/*
	 * Order previous accesses before accesses in the IPI handler.
	 */
	smp_mb();
	message[msg] = 1;
	/*
	 * cause_ipi functions are required to include a full barrier
	 * before doing whatever causes the IPI.
	 */
	smp_ops->cause_ipi(cpu, info->data);
}

#ifdef __BIG_ENDIAN__
#define IPI_MESSAGE(A) (1 << (24 - 8 * (A)))
#else
#define IPI_MESSAGE(A) (1 << (8 * (A)))
#endif

irqreturn_t smp_ipi_demux(void)
{
	struct cpu_messages *info = this_cpu_ptr(&ipi_message);
	unsigned int all;

	mb();	/* order any irq clear */

	do {
		all = xchg(&info->messages, 0);
		if (all & IPI_MESSAGE(PPC_MSG_CALL_FUNCTION))
			generic_smp_call_function_interrupt();
		if (all & IPI_MESSAGE(PPC_MSG_RESCHEDULE))
			scheduler_ipi();
		if (all & IPI_MESSAGE(PPC_MSG_TICK_BROADCAST))
			tick_broadcast_ipi_handler();
		if (all & IPI_MESSAGE(PPC_MSG_DEBUGGER_BREAK))
			debug_ipi_action(0, NULL);
	} while (info->messages);

	return IRQ_HANDLED;
}
#endif /* CONFIG_PPC_SMP_MUXED_IPI */

static inline void do_message_pass(int cpu, int msg)
{
	if (smp_ops->message_pass)
		smp_ops->message_pass(cpu, msg);
#ifdef CONFIG_PPC_SMP_MUXED_IPI
	else
		smp_muxed_ipi_message_pass(cpu, msg);
#endif
}

void smp_send_reschedule(int cpu)
{
	if (likely(smp_ops))
		do_message_pass(cpu, PPC_MSG_RESCHEDULE);
}
EXPORT_SYMBOL_GPL(smp_send_reschedule);

void arch_send_call_function_single_ipi(int cpu)
{
	do_message_pass(cpu, PPC_MSG_CALL_FUNCTION);
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		do_message_pass(cpu, PPC_MSG_CALL_FUNCTION);
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
void tick_broadcast(const struct cpumask *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		do_message_pass(cpu, PPC_MSG_TICK_BROADCAST);
}
#endif

#if defined(CONFIG_DEBUGGER) || defined(CONFIG_KEXEC)
void smp_send_debugger_break(void)
{
	int cpu;
	int me = raw_smp_processor_id();

	if (unlikely(!smp_ops))
		return;

	for_each_online_cpu(cpu)
		if (cpu != me)
			do_message_pass(cpu, PPC_MSG_DEBUGGER_BREAK);
}
#endif

#ifdef CONFIG_KEXEC
void crash_send_ipi(void (*crash_ipi_callback)(struct pt_regs *))
{
	crash_ipi_function_ptr = crash_ipi_callback;
	if (crash_ipi_callback) {
		mb();
		smp_send_debugger_break();
	}
}
#endif

static void stop_this_cpu(void *dummy)
{
	/* Remove this CPU */
	set_cpu_online(smp_processor_id(), false);

	local_irq_disable();
	while (1)
		;
}

void smp_send_stop(void)
{
	smp_call_function(stop_this_cpu, NULL, 0);
}

struct thread_info *current_set[NR_CPUS];

static void smp_store_cpu_info(int id)
{
	per_cpu(cpu_pvr, id) = mfspr(SPRN_PVR);
#ifdef CONFIG_PPC_FSL_BOOK3E
	per_cpu(next_tlbcam_idx, id)
		= (mfspr(SPRN_TLB1CFG) & TLBnCFG_N_ENTRY) - 1;
#endif
}

/*
 * Relationships between CPUs are maintained in a set of per-cpu cpumasks so
 * rather than just passing around the cpumask we pass around a function that
 * returns the that cpumask for the given CPU.
 */
static void set_cpus_related(int i, int j, struct cpumask *(*get_cpumask)(int))
{
	cpumask_set_cpu(i, get_cpumask(j));
	cpumask_set_cpu(j, get_cpumask(i));
}

#ifdef CONFIG_HOTPLUG_CPU
static void set_cpus_unrelated(int i, int j,
		struct cpumask *(*get_cpumask)(int))
{
	cpumask_clear_cpu(i, get_cpumask(j));
	cpumask_clear_cpu(j, get_cpumask(i));
}
#endif

/*
 * parse_thread_groups: Parses the "ibm,thread-groups" device tree
 *                      property for the CPU device node @dn and stores
 *                      the parsed output in the thread_groups
 *                      structure @tg if the ibm,thread-groups[0]
 *                      matches @property.
 *
 * @dn: The device node of the CPU device.
 * @tg: Pointer to a thread group structure into which the parsed
 *      output of "ibm,thread-groups" is stored.
 * @property: The property of the thread-group that the caller is
 *            interested in.
 *
 * ibm,thread-groups[0..N-1] array defines which group of threads in
 * the CPU-device node can be grouped together based on the property.
 *
 * ibm,thread-groups[0] tells us the property based on which the
 * threads are being grouped together. If this value is 1, it implies
 * that the threads in the same group share L1, translation cache.
 *
 * ibm,thread-groups[1] tells us how many such thread groups exist.
 *
 * ibm,thread-groups[2] tells us the number of threads in each such
 * group.
 *
 * ibm,thread-groups[3..N-1] is the list of threads identified by
 * "ibm,ppc-interrupt-server#s" arranged as per their membership in
 * the grouping.
 *
 * Example: If ibm,thread-groups = [1,2,4,5,6,7,8,9,10,11,12] it
 * implies that there are 2 groups of 4 threads each, where each group
 * of threads share L1, translation cache.
 *
 * The "ibm,ppc-interrupt-server#s" of the first group is {5,6,7,8}
 * and the "ibm,ppc-interrupt-server#s" of the second group is {9, 10,
 * 11, 12} structure
 *
 * Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 */
static int parse_thread_groups(struct device_node *dn,
			       struct thread_groups *tg,
			       unsigned int property)
{
	int i;
	u32 thread_group_array[3 + MAX_THREAD_LIST_SIZE];
	u32 *thread_list;
	size_t total_threads;
	int ret;

	ret = of_property_read_u32_array(dn, "ibm,thread-groups",
					 thread_group_array, 3);
	if (ret)
		return ret;

	tg->property = thread_group_array[0];
	tg->nr_groups = thread_group_array[1];
	tg->threads_per_group = thread_group_array[2];
	if (tg->property != property ||
	    tg->nr_groups < 1 ||
	    tg->threads_per_group < 1)
		return -ENODATA;

	total_threads = tg->nr_groups * tg->threads_per_group;

	ret = of_property_read_u32_array(dn, "ibm,thread-groups",
					 thread_group_array,
					 3 + total_threads);
	if (ret)
		return ret;

	thread_list = &thread_group_array[3];

	for (i = 0 ; i < total_threads; i++)
		tg->thread_list[i] = thread_list[i];

	return 0;
}

/*
 * get_cpu_thread_group_start : Searches the thread group in tg->thread_list
 *                              that @cpu belongs to.
 *
 * @cpu : The logical CPU whose thread group is being searched.
 * @tg : The thread-group structure of the CPU node which @cpu belongs
 *       to.
 *
 * Returns the index to tg->thread_list that points to the the start
 * of the thread_group that @cpu belongs to.
 *
 * Returns -1 if cpu doesn't belong to any of the groups pointed to by
 * tg->thread_list.
 */
static int get_cpu_thread_group_start(int cpu, struct thread_groups *tg)
{
	int hw_cpu_id = get_hard_smp_processor_id(cpu);
	int i, j;

	for (i = 0; i < tg->nr_groups; i++) {
		int group_start = i * tg->threads_per_group;

		for (j = 0; j < tg->threads_per_group; j++) {
			int idx = group_start + j;

			if (tg->thread_list[idx] == hw_cpu_id)
				return group_start;
		}
	}

	return -1;
}

static int init_cpu_l1_cache_map(int cpu)

{
	struct device_node *dn = of_get_cpu_node(cpu, NULL);
	struct thread_groups tg = {.property = 0,
				   .nr_groups = 0,
				   .threads_per_group = 0};
	int first_thread = cpu_first_thread_sibling(cpu);
	int i, cpu_group_start = -1, err = 0;

	if (!dn)
		return -ENODATA;

	err = parse_thread_groups(dn, &tg, THREAD_GROUP_SHARE_L1);
	if (err)
		goto out;

	zalloc_cpumask_var_node(&per_cpu(cpu_l1_cache_map, cpu),
				GFP_KERNEL,
				cpu_to_node(cpu));

	cpu_group_start = get_cpu_thread_group_start(cpu, &tg);

	if (unlikely(cpu_group_start == -1)) {
		WARN_ON_ONCE(1);
		err = -ENODATA;
		goto out;
	}

	for (i = first_thread; i < first_thread + threads_per_core; i++) {
		int i_group_start = get_cpu_thread_group_start(i, &tg);

		if (unlikely(i_group_start == -1)) {
			WARN_ON_ONCE(1);
			err = -ENODATA;
			goto out;
		}

		if (i_group_start == cpu_group_start)
			cpumask_set_cpu(i, per_cpu(cpu_l1_cache_map, cpu));
	}

out:
	of_node_put(dn);
	return err;
}

static int init_big_cores(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int err = init_cpu_l1_cache_map(cpu);

		if (err)
			return err;

		zalloc_cpumask_var_node(&per_cpu(cpu_smallcore_map, cpu),
					GFP_KERNEL,
					cpu_to_node(cpu));
	}

	has_big_cores = true;
	return 0;
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	unsigned int cpu;

	DBG("smp_prepare_cpus\n");

	/* 
	 * setup_cpu may need to be called on the boot cpu. We havent
	 * spun any cpus up but lets be paranoid.
	 */
	BUG_ON(boot_cpuid != smp_processor_id());

	/* Fixup boot cpu */
	smp_store_cpu_info(boot_cpuid);
	cpu_callin_map[boot_cpuid] = 1;

	for_each_possible_cpu(cpu) {
		zalloc_cpumask_var_node(&per_cpu(cpu_sibling_map, cpu),
					GFP_KERNEL, cpu_to_node(cpu));
		zalloc_cpumask_var_node(&per_cpu(cpu_l2_cache_map, cpu),
					GFP_KERNEL, cpu_to_node(cpu));
		zalloc_cpumask_var_node(&per_cpu(cpu_core_map, cpu),
					GFP_KERNEL, cpu_to_node(cpu));
		/*
		 * numa_node_id() works after this.
		 */
		if (cpu_present(cpu)) {
			set_cpu_numa_node(cpu, numa_cpu_lookup_table[cpu]);
			set_cpu_numa_mem(cpu,
				local_memory_node(numa_cpu_lookup_table[cpu]));
		}
	}

	/* Init the cpumasks so the boot CPU is related to itself */
	cpumask_set_cpu(boot_cpuid, cpu_sibling_mask(boot_cpuid));
	cpumask_set_cpu(boot_cpuid, cpu_l2_cache_mask(boot_cpuid));
	cpumask_set_cpu(boot_cpuid, cpu_core_mask(boot_cpuid));

	init_big_cores();
	if (has_big_cores) {
		cpumask_set_cpu(boot_cpuid,
				cpu_smallcore_mask(boot_cpuid));
	}

	if (smp_ops && smp_ops->probe)
		smp_ops->probe();
}

void smp_prepare_boot_cpu(void)
{
	BUG_ON(smp_processor_id() != boot_cpuid);
#ifdef CONFIG_PPC64
	paca[boot_cpuid].__current = current;
#endif
	set_numa_node(numa_cpu_lookup_table[boot_cpuid]);
	current_set[boot_cpuid] = task_thread_info(current);
}

#ifdef CONFIG_HOTPLUG_CPU

int generic_cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();

	if (cpu == boot_cpuid)
		return -EBUSY;

	set_cpu_online(cpu, false);
#ifdef CONFIG_PPC64
	vdso_data->processorCount--;
#endif
	migrate_irqs();
	return 0;
}

void generic_cpu_die(unsigned int cpu)
{
	int i;

	for (i = 0; i < 100; i++) {
		smp_rmb();
		if (per_cpu(cpu_state, cpu) == CPU_DEAD)
			return;
		msleep(100);
	}
	printk(KERN_ERR "CPU%d didn't die...\n", cpu);
}

void generic_set_cpu_dead(unsigned int cpu)
{
	per_cpu(cpu_state, cpu) = CPU_DEAD;
}

/*
 * The cpu_state should be set to CPU_UP_PREPARE in kick_cpu(), otherwise
 * the cpu_state is always CPU_DEAD after calling generic_set_cpu_dead(),
 * which makes the delay in generic_cpu_die() not happen.
 */
void generic_set_cpu_up(unsigned int cpu)
{
	per_cpu(cpu_state, cpu) = CPU_UP_PREPARE;
}

int generic_check_cpu_restart(unsigned int cpu)
{
	return per_cpu(cpu_state, cpu) == CPU_UP_PREPARE;
}

static bool secondaries_inhibited(void)
{
	return kvm_hv_mode_active();
}

#else /* HOTPLUG_CPU */

#define secondaries_inhibited()		0

#endif

static void cpu_idle_thread_init(unsigned int cpu, struct task_struct *idle)
{
	struct thread_info *ti = task_thread_info(idle);

#ifdef CONFIG_PPC64
	paca[cpu].__current = idle;
	paca[cpu].kstack = (unsigned long)ti + THREAD_SIZE - STACK_FRAME_OVERHEAD;
#endif
	ti->cpu = cpu;
	secondary_ti = current_set[cpu] = ti;
}

int __cpu_up(unsigned int cpu, struct task_struct *tidle)
{
	int rc, c;

	/*
	 * Don't allow secondary threads to come online if inhibited
	 */
	if (threads_per_core > 1 && secondaries_inhibited() &&
	    cpu_thread_in_subcore(cpu))
		return -EBUSY;

	if (smp_ops == NULL ||
	    (smp_ops->cpu_bootable && !smp_ops->cpu_bootable(cpu)))
		return -EINVAL;

	cpu_idle_thread_init(cpu, tidle);

	/* Make sure callin-map entry is 0 (can be leftover a CPU
	 * hotplug
	 */
	cpu_callin_map[cpu] = 0;

	/* The information for processor bringup must
	 * be written out to main store before we release
	 * the processor.
	 */
	smp_mb();

	/* wake up cpus */
	DBG("smp: kicking cpu %d\n", cpu);
	rc = smp_ops->kick_cpu(cpu);
	if (rc) {
		pr_err("smp: failed starting cpu %d (rc %d)\n", cpu, rc);
		return rc;
	}

	/*
	 * wait to see if the cpu made a callin (is actually up).
	 * use this value that I found through experimentation.
	 * -- Cort
	 */
	if (system_state < SYSTEM_RUNNING)
		for (c = 50000; c && !cpu_callin_map[cpu]; c--)
			udelay(100);
#ifdef CONFIG_HOTPLUG_CPU
	else
		/*
		 * CPUs can take much longer to come up in the
		 * hotplug case.  Wait five seconds.
		 */
		for (c = 5000; c && !cpu_callin_map[cpu]; c--)
			msleep(1);
#endif

	if (!cpu_callin_map[cpu]) {
		printk(KERN_ERR "Processor %u is stuck.\n", cpu);
		return -ENOENT;
	}

	DBG("Processor %u found.\n", cpu);

	if (smp_ops->give_timebase)
		smp_ops->give_timebase();

	/* Wait until cpu puts itself in the online & active maps */
	while (!cpu_online(cpu) || !cpu_active(cpu))
		cpu_relax();

	return 0;
}

/* Return the value of the reg property corresponding to the given
 * logical cpu.
 */
int cpu_to_core_id(int cpu)
{
	struct device_node *np;
	const __be32 *reg;
	int id = -1;

	np = of_get_cpu_node(cpu, NULL);
	if (!np)
		goto out;

	reg = of_get_property(np, "reg", NULL);
	if (!reg)
		goto out;

	id = be32_to_cpup(reg);
out:
	of_node_put(np);
	return id;
}

/* Helper routines for cpu to core mapping */
int cpu_core_index_of_thread(int cpu)
{
	return cpu >> threads_shift;
}
EXPORT_SYMBOL_GPL(cpu_core_index_of_thread);

int cpu_first_thread_of_core(int core)
{
	return core << threads_shift;
}
EXPORT_SYMBOL_GPL(cpu_first_thread_of_core);

/* Must be called when no change can occur to cpu_present_mask,
 * i.e. during cpu online or offline.
 */
static struct device_node *cpu_to_l2cache(int cpu)
{
	struct device_node *np;
	struct device_node *cache;

	if (!cpu_present(cpu))
		return NULL;

	np = of_get_cpu_node(cpu, NULL);
	if (np == NULL)
		return NULL;

	cache = of_find_next_cache_node(np);

	of_node_put(np);

	return cache;
}

static bool update_mask_by_l2(int cpu, struct cpumask *(*mask_fn)(int))
{
	struct device_node *l2_cache, *np;
	int i;

	l2_cache = cpu_to_l2cache(cpu);
	if (!l2_cache)
		return false;

	for_each_cpu(i, cpu_online_mask) {
		/*
		 * when updating the marks the current CPU has not been marked
		 * online, but we need to update the cache masks
		 */
		np = cpu_to_l2cache(i);
		if (!np)
			continue;

		if (np == l2_cache)
			set_cpus_related(cpu, i, mask_fn);

		of_node_put(np);
	}
	of_node_put(l2_cache);

	return true;
}

#ifdef CONFIG_HOTPLUG_CPU
static void remove_cpu_from_masks(int cpu)
{
	int i;

	/* NB: cpu_core_mask is a superset of the others */
	for_each_cpu(i, cpu_core_mask(cpu)) {
		set_cpus_unrelated(cpu, i, cpu_core_mask);
		set_cpus_unrelated(cpu, i, cpu_l2_cache_mask);
		set_cpus_unrelated(cpu, i, cpu_sibling_mask);
		if (has_big_cores)
			set_cpus_unrelated(cpu, i, cpu_smallcore_mask);
	}
}
#endif

static inline void add_cpu_to_smallcore_masks(int cpu)
{
	struct cpumask *this_l1_cache_map = per_cpu(cpu_l1_cache_map, cpu);
	int i, first_thread = cpu_first_thread_sibling(cpu);

	if (!has_big_cores)
		return;

	cpumask_set_cpu(cpu, cpu_smallcore_mask(cpu));

	for (i = first_thread; i < first_thread + threads_per_core; i++) {
		if (cpu_online(i) && cpumask_test_cpu(i, this_l1_cache_map))
			set_cpus_related(i, cpu, cpu_smallcore_mask);
	}
}

static void add_cpu_to_masks(int cpu)
{
	int first_thread = cpu_first_thread_sibling(cpu);
	int chipid = cpu_to_chip_id(cpu);
	int i;

	/*
	 * This CPU will not be in the online mask yet so we need to manually
	 * add it to it's own thread sibling mask.
	 */
	cpumask_set_cpu(cpu, cpu_sibling_mask(cpu));

	for (i = first_thread; i < first_thread + threads_per_core; i++)
		if (cpu_online(i))
			set_cpus_related(i, cpu, cpu_sibling_mask);

	add_cpu_to_smallcore_masks(cpu);
	/*
	 * Copy the thread sibling mask into the cache sibling mask
	 * and mark any CPUs that share an L2 with this CPU.
	 */
	for_each_cpu(i, cpu_sibling_mask(cpu))
		set_cpus_related(cpu, i, cpu_l2_cache_mask);
	update_mask_by_l2(cpu, cpu_l2_cache_mask);

	/*
	 * Copy the cache sibling mask into core sibling mask and mark
	 * any CPUs on the same chip as this CPU.
	 */
	for_each_cpu(i, cpu_l2_cache_mask(cpu))
		set_cpus_related(cpu, i, cpu_core_mask);

	if (chipid == -1)
		return;

	for_each_cpu(i, cpu_online_mask)
		if (cpu_to_chip_id(i) == chipid)
			set_cpus_related(cpu, i, cpu_core_mask);
}

static bool shared_caches;

/* Activate a secondary processor. */
void start_secondary(void *unused)
{
	unsigned int cpu = smp_processor_id();
	struct cpumask *(*sibling_mask)(int) = cpu_sibling_mask;

	atomic_inc(&init_mm.mm_count);
	current->active_mm = &init_mm;

	smp_store_cpu_info(cpu);
	set_dec(tb_ticks_per_jiffy);
	preempt_disable();
	cpu_callin_map[cpu] = 1;

	if (smp_ops->setup_cpu)
		smp_ops->setup_cpu(cpu);
	if (smp_ops->take_timebase)
		smp_ops->take_timebase();

	secondary_cpu_time_init();

#ifdef CONFIG_PPC64
	if (system_state == SYSTEM_RUNNING)
		vdso_data->processorCount++;

	vdso_getcpu_init();
#endif
	/* Update topology CPU masks */
	add_cpu_to_masks(cpu);

	if (has_big_cores)
		sibling_mask = cpu_smallcore_mask;
	/*
	 * Check for any shared caches. Note that this must be done on a
	 * per-core basis because one core in the pair might be disabled.
	 */
	if (!cpumask_equal(cpu_l2_cache_mask(cpu), sibling_mask(cpu)))
		shared_caches = true;

	set_numa_node(numa_cpu_lookup_table[cpu]);
	set_numa_mem(local_memory_node(numa_cpu_lookup_table[cpu]));

	smp_wmb();
	notify_cpu_starting(cpu);
	set_cpu_online(cpu, true);

	local_irq_enable();

	cpu_startup_entry(CPUHP_ONLINE);

	BUG();
}

int setup_profiling_timer(unsigned int multiplier)
{
	return 0;
}

#ifdef CONFIG_SCHED_SMT
/* cpumask of CPUs with asymetric SMT dependancy */
static int powerpc_smt_flags(void)
{
	int flags = SD_SHARE_CPUCAPACITY | SD_SHARE_PKG_RESOURCES;

	if (cpu_has_feature(CPU_FTR_ASYM_SMT)) {
		printk_once(KERN_INFO "Enabling Asymmetric SMT scheduling\n");
		flags |= SD_ASYM_PACKING;
	}
	return flags;
}
#endif

static struct sched_domain_topology_level powerpc_topology[] = {
#ifdef CONFIG_SCHED_SMT
	{ cpu_smt_mask, powerpc_smt_flags, SD_INIT_NAME(SMT) },
#endif
	{ cpu_cpu_mask, SD_INIT_NAME(DIE) },
	{ NULL, },
};

/*
 * P9 has a slightly odd architecture where pairs of cores share an L2 cache.
 * This topology makes it *much* cheaper to migrate tasks between adjacent cores
 * since the migrated task remains cache hot. We want to take advantage of this
 * at the scheduler level so an extra topology level is required.
 */
static int powerpc_shared_cache_flags(void)
{
	return SD_SHARE_PKG_RESOURCES;
}

/*
 * We can't just pass cpu_l2_cache_mask() directly because
 * returns a non-const pointer and the compiler barfs on that.
 */
static const struct cpumask *shared_cache_mask(int cpu)
{
	return cpu_l2_cache_mask(cpu);
}

#ifdef CONFIG_SCHED_SMT
static const struct cpumask *smallcore_smt_mask(int cpu)
{
	return cpu_smallcore_mask(cpu);
}
#endif

static struct sched_domain_topology_level power9_topology[] = {
#ifdef CONFIG_SCHED_SMT
	{ cpu_smt_mask, powerpc_smt_flags, SD_INIT_NAME(SMT) },
#endif
	{ shared_cache_mask, powerpc_shared_cache_flags, SD_INIT_NAME(CACHE) },
	{ cpu_cpu_mask, SD_INIT_NAME(DIE) },
	{ NULL, },
};

void __init smp_cpus_done(unsigned int max_cpus)
{
	cpumask_var_t old_mask;

	/* We want the setup_cpu() here to be called from CPU 0, but our
	 * init thread may have been "borrowed" by another CPU in the meantime
	 * se we pin us down to CPU 0 for a short while
	 */
	alloc_cpumask_var(&old_mask, GFP_NOWAIT);
	cpumask_copy(old_mask, tsk_cpus_allowed(current));
	set_cpus_allowed_ptr(current, cpumask_of(boot_cpuid));
	
	if (smp_ops && smp_ops->setup_cpu)
		smp_ops->setup_cpu(boot_cpuid);

	set_cpus_allowed_ptr(current, old_mask);

	free_cpumask_var(old_mask);

	if (smp_ops && smp_ops->bringup_done)
		smp_ops->bringup_done();

	/*
	 * On a shared LPAR, associativity needs to be requested.
	 * Hence, get numa topology before dumping cpu topology
	 */
	shared_proc_topology_init();
	dump_numa_cpu_topology();

#ifdef CONFIG_SCHED_SMT
	if (has_big_cores) {
		pr_info("Using small cores at SMT level\n");
		power9_topology[0].mask = smallcore_smt_mask;
		powerpc_topology[0].mask = smallcore_smt_mask;
	}
#endif
	/*
	 * If any CPU detects that it's sharing a cache with another CPU then
	 * use the deeper topology that is aware of this sharing.
	 */
	if (shared_caches) {
		pr_info("Using shared cache scheduler topology\n");
		set_sched_topology(power9_topology);
	} else {
		pr_info("Using standard scheduler topology\n");
		set_sched_topology(powerpc_topology);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
int __cpu_disable(void)
{
	int cpu = smp_processor_id();
	int err;

	if (!smp_ops->cpu_disable)
		return -ENOSYS;

	err = smp_ops->cpu_disable();
	if (err)
		return err;

	/* Update sibling maps */
	remove_cpu_from_masks(cpu);

	return 0;
}

void __cpu_die(unsigned int cpu)
{
	if (smp_ops->cpu_die)
		smp_ops->cpu_die(cpu);
}

void cpu_die(void)
{
	if (ppc_md.cpu_die)
		ppc_md.cpu_die();

	/* If we return, we re-enter start_secondary */
	start_secondary_resume();
}

#endif
