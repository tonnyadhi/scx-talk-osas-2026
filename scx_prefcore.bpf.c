/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_prefcore - Stage 3: a toy scheduler that knows AMD preferred cores.
 *
 * Modern AMD parts do not have identical cores: amd-pstate exposes a
 * per-CPU "preferred core ranking" (higher = the silicon lottery winner,
 * capable of higher boost clocks). The stock scheduler feeds this into
 * ITMT; here we consume it explicitly.
 *
 * Policy:
 *   - Userspace reads the ranking from sysfs at startup and bakes it into
 *     this program's read-only data (cpu_order[], strong_cpu[]) before
 *     the verifier runs. That is the second BPF<->userspace channel:
 *     rodata as load-time configuration.
 *   - Tasks are classified online. A per-task EWMA of how much of its
 *     slice a task actually uses:
 *         short runs, sleeps a lot  -> "latency" class (interactive)
 *         burns whole slices        -> "batch" class
 *   - Two custom DSQs. Latency tasks queue on DSQ_LATENCY and are steered
 *     toward the strongest cores; batch tasks queue on DSQ_BATCH and are
 *     parked on the weaker cores. Each tier services the other's queue
 *     when its own is empty, so nothing starves.
 *   - The kernel's cpuperf interface (scx_bpf_cpuperf_cur/_cap) lets us
 *     observe the performance level each core is actually running at;
 *     we sample it every tick so userspace can show that latency work
 *     really lands on the fast cores.
 *
 * This is pedagogy, not production: no NUMA/LLC awareness, no
 * kick-on-enqueue, coarse classification. scx_lavd/scx_rusty show the
 * grown-up versions of every idea here.
 *
 * Build: drop this pair into the scx repo's scheds/c/ and add "scx_prefcore"
 * to scheds/c/meson.build — full per-distro steps in README.md.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

#define MAX_CPUS	512

#define DSQ_LATENCY	0
#define DSQ_BATCH	1

UEI_DEFINE(uei);

/*
 * Load-time configuration, filled in by userspace between skeleton open
 * and load ("const volatile" => rodata => the verifier sees the final
 * values as constants).
 */
const volatile u32 nr_cpus = 1;
/* CPU ids sorted best-ranked first */
const volatile s32 cpu_order[MAX_CPUS];
/* 1 if this CPU is in the strong tier */
const volatile u8 strong_cpu[MAX_CPUS];
/* below this average per-run CPU usage a task counts as latency-bound */
const volatile u64 lat_thresh_ns = 1000 * 1000;	/* 1ms */

/* Per-task state, attached to the task_struct via task local storage. */
struct task_ctx {
	u64 avg_used_ns;	/* EWMA of slice actually consumed per run */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

enum stat_idx {
	STAT_DIRECT,	/* wakeup found an idle CPU, skipped the queues */
	STAT_LAT_ENQ,	/* enqueued on DSQ_LATENCY */
	STAT_BATCH_ENQ,	/* enqueued on DSQ_BATCH */
	STAT_XTIER,	/* a tier had to service the other tier's queue */
	NR_STATS,
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, NR_STATS);
} stats SEC(".maps");

/* Last observed cpuperf level per CPU (0..SCX_CPUPERF_ONE), for demo. */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u32));
	__uint(max_entries, MAX_CPUS);
} cpuperf_sample SEC(".maps");

static void stat_inc(u32 idx)
{
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);

	if (cnt_p)
		(*cnt_p)++;
}

static struct task_ctx *lookup_task_ctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

/*
 * The classifier. New tasks have avg_used_ns == 0 and therefore start in
 * the latency class - optimistic, like most schedulers - and drift to
 * batch as they demonstrate appetite.
 */
static bool task_is_latency(struct task_struct *p)
{
	struct task_ctx *tctx = lookup_task_ctx(p);

	if (!tctx)
		return true;
	return tctx->avg_used_ns < lat_thresh_ns;
}

/*
 * ops.select_cpu - wakeup path. Instead of the default "any idle CPU near
 * the last one", walk the CPUs in *ranking order*: latency tasks scan
 * best->worst, batch tasks scan worst->best. First idle, allowed CPU
 * wins and the task is dispatched directly to it.
 */
s32 BPF_STRUCT_OPS(prefcore_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	bool lat = task_is_latency(p);
	u32 i;

	if (p->nr_cpus_allowed == 1)
		return prev_cpu;

	bpf_for(i, 0, nr_cpus) {
		u32 idx = lat ? i : nr_cpus - 1 - i;
		s32 cpu;

		if (idx >= MAX_CPUS)	/* keep the verifier happy */
			break;
		cpu = cpu_order[idx];
		if (cpu < 0 || cpu >= MAX_CPUS)
			continue;
		if (!bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
			continue;
		if (scx_bpf_test_and_clear_cpu_idle(cpu)) {
			stat_inc(STAT_DIRECT);
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
			return cpu;
		}
	}
	return prev_cpu;
}

/*
 * ops.enqueue - no idle CPU: queue on the class DSQ. Which physical CPU
 * eventually runs the task is decided by *which CPUs consume which DSQ*
 * in ops.dispatch below - that is the whole steering mechanism.
 */
void BPF_STRUCT_OPS(prefcore_enqueue, struct task_struct *p, u64 enq_flags)
{
	if (task_is_latency(p)) {
		stat_inc(STAT_LAT_ENQ);
		scx_bpf_dsq_insert(p, DSQ_LATENCY, SCX_SLICE_DFL, enq_flags);
	} else {
		stat_inc(STAT_BATCH_ENQ);
		scx_bpf_dsq_insert(p, DSQ_BATCH, SCX_SLICE_DFL, enq_flags);
	}
}

/*
 * ops.dispatch - the asymmetry lives here. Strong cores prefer the
 * latency queue; weak cores prefer the batch queue. Each falls back to
 * the other so work conservation holds: an idle strong core will happily
 * chew batch work rather than sit idle, and vice versa.
 */
void BPF_STRUCT_OPS(prefcore_dispatch, s32 cpu, struct task_struct *prev)
{
	bool strong = cpu >= 0 && cpu < MAX_CPUS && strong_cpu[cpu];

	if (strong) {
		if (scx_bpf_dsq_move_to_local(DSQ_LATENCY))
			return;
		if (scx_bpf_dsq_move_to_local(DSQ_BATCH))
			stat_inc(STAT_XTIER);
	} else {
		if (scx_bpf_dsq_move_to_local(DSQ_BATCH))
			return;
		if (scx_bpf_dsq_move_to_local(DSQ_LATENCY))
			stat_inc(STAT_XTIER);
	}
}

/*
 * ops.stopping - update the classifier. p->scx.slice is what remains of
 * the slice we granted, so (granted - remaining) is what the task burned
 * this time on the CPU. EWMA with alpha = 1/4.
 */
void BPF_STRUCT_OPS(prefcore_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx = lookup_task_ctx(p);
	u64 used;

	if (!tctx)
		return;
	used = SCX_SLICE_DFL - p->scx.slice;
	tctx->avg_used_ns = (3 * tctx->avg_used_ns + used) / 4;
}

/*
 * ops.tick - once per timer tick on each busy CPU. We sample the cpuperf
 * interface: scx_bpf_cpuperf_cur() reports the performance level the CPU
 * is currently asked to run at, scaled so SCX_CPUPERF_ONE == 1024 ==
 * "maximum". (A sibling kfunc, scx_bpf_cpuperf_set(), would let this
 * scheduler drive frequency selection itself - we only observe.)
 */
void BPF_STRUCT_OPS(prefcore_tick, struct task_struct *p)
{
	u32 cpu = bpf_get_smp_processor_id();
	u32 *slot;

	slot = bpf_map_lookup_elem(&cpuperf_sample, &cpu);
	if (slot)
		*slot = scx_bpf_cpuperf_cur(cpu);
}

/*
 * ops.init_task - a task is entering our jurisdiction (fork or scheduler
 * load). Create its storage. Returning an error here aborts the fork /
 * the scheduler load, so this is also the resource-attribution point.
 */
s32 BPF_STRUCT_OPS(prefcore_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct task_ctx *tctx;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;
	tctx->avg_used_ns = 0;
	return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(prefcore_init)
{
	s32 ret;

	ret = scx_bpf_create_dsq(DSQ_LATENCY, -1);
	if (ret)
		return ret;
	return scx_bpf_create_dsq(DSQ_BATCH, -1);
}

void BPF_STRUCT_OPS(prefcore_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(prefcore_ops,
	       .select_cpu		= (void *)prefcore_select_cpu,
	       .enqueue			= (void *)prefcore_enqueue,
	       .dispatch		= (void *)prefcore_dispatch,
	       .stopping		= (void *)prefcore_stopping,
	       .tick			= (void *)prefcore_tick,
	       .init_task		= (void *)prefcore_init_task,
	       .init			= (void *)prefcore_init,
	       .exit			= (void *)prefcore_exit,
	       .name			= "prefcore");
