/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_toydsq - Stage 2: custom DSQs, an idle-CPU fast path, weighted
 * fairness, and talking to userspace through maps.
 *
 * New concepts over stage 1:
 *   1. Custom DSQs: we create our own queue (SHARED_DSQ) in ops.init.
 *      Unlike SCX_DSQ_GLOBAL, custom DSQs are NOT consumed automatically -
 *      ops.dispatch is now load-bearing.
 *   2. ops.select_cpu: runs on wakeup, before enqueue. If we find an idle
 *      CPU we can insert the task directly into that CPU's local DSQ and
 *      skip the shared queue entirely (ops.enqueue is then not called).
 *   3. Virtual time: a custom DSQ can be a priority queue ordered by
 *      "vtime". Heavier tasks accrue vtime slower => weighted fairness in
 *      ~15 lines. This is the same idea CFS/EEVDF are built on.
 *   4. Stats via a per-CPU array map that userspace reads every second.
 *
 * Build: drop this pair into the scx repo's scheds/c/ and add "scx_toydsq"
 * to scheds/c/meson.build — full per-distro steps in README.md.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

#define SHARED_DSQ 0

UEI_DEFINE(uei);

/*
 * Global vtime clock: the vtime of the task that most recently started
 * running. New/woken tasks are clamped near it so a task that slept for
 * an hour cannot come back and monopolize the CPU "repaying" its debt.
 */
static u64 vtime_now;

/* stats[0] = direct dispatches to an idle CPU, stats[1] = shared-queue */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 2);
} stats SEC(".maps");

static void stat_inc(u32 idx)
{
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);

	if (cnt_p)
		(*cnt_p)++;
}

static inline bool vtime_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

/*
 * ops.select_cpu - wakeup fast path. scx_bpf_select_cpu_dfl() is the
 * built-in idle-CPU picker (prev CPU if idle, then same LLC, etc.). If it
 * found an idle CPU we dispatch straight to its local DSQ: lowest possible
 * wakeup latency, no contention on the shared queue.
 */
s32 BPF_STRUCT_OPS(toydsq_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		stat_inc(0);	/* direct dispatch */
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	}
	return cpu;
}

/*
 * ops.enqueue - no idle CPU was found, so the task waits in the shared
 * DSQ. scx_bpf_dsq_insert_vtime() makes the DSQ a priority queue keyed on
 * p->scx.dsq_vtime: smallest vtime (i.e. the task that has received the
 * least weighted CPU time) is consumed first.
 */
void BPF_STRUCT_OPS(toydsq_enqueue, struct task_struct *p, u64 enq_flags)
{
	u64 vtime = p->scx.dsq_vtime;

	stat_inc(1);	/* queued on shared DSQ */

	/*
	 * Clamp idle-time credit to one full slice. A task can be at most
	 * one slice "in the past" - enough to win the next dispatch, not
	 * enough to starve everyone afterwards.
	 */
	if (vtime_before(vtime, vtime_now - SCX_SLICE_DFL))
		vtime = vtime_now - SCX_SLICE_DFL;

	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, vtime,
				 enq_flags);
}

/*
 * ops.dispatch - CPU is hungry. Refill its local DSQ from the shared
 * priority queue. Mandatory now: nobody else consumes a custom DSQ.
 */
void BPF_STRUCT_OPS(toydsq_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

/*
 * ops.running - task p was just put on a CPU. Advance the global vtime
 * clock so sleepers wake up "now", not in the past.
 */
void BPF_STRUCT_OPS(toydsq_running, struct task_struct *p)
{
	if (vtime_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
}

/*
 * ops.stopping - task p is coming off a CPU. Charge it for the slice it
 * actually used, scaled inversely by weight: at nice 0 (weight 100) real
 * time == vtime; a heavier task's vtime advances slower, so it gets
 * proportionally more CPU. This one line is the whole fairness policy.
 */
void BPF_STRUCT_OPS(toydsq_stopping, struct task_struct *p, bool runnable)
{
	p->scx.dsq_vtime +=
		(SCX_SLICE_DFL - p->scx.slice) * 100 / p->scx.weight;
}

/*
 * ops.enable - task p starts being scheduled by us (fork, or scheduler
 * load). Give it a sane starting vtime.
 */
void BPF_STRUCT_OPS(toydsq_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = vtime_now;
}

/*
 * ops.init - create our DSQ before any task is enqueued. Sleepable
 * (SCX_OPS_DEFINE handles the program type); -1 = any NUMA node.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(toydsq_init)
{
	return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

void BPF_STRUCT_OPS(toydsq_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(toydsq_ops,
	       .select_cpu		= (void *)toydsq_select_cpu,
	       .enqueue			= (void *)toydsq_enqueue,
	       .dispatch		= (void *)toydsq_dispatch,
	       .running			= (void *)toydsq_running,
	       .stopping		= (void *)toydsq_stopping,
	       .enable			= (void *)toydsq_enable,
	       .init			= (void *)toydsq_init,
	       .exit			= (void *)toydsq_exit,
	       .name			= "toydsq");
