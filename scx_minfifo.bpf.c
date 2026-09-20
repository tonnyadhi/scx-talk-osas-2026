/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_minfifo - Stage 1: the smallest scheduler that can run your machine.
 *
 * Teaching goals:
 *   1. The sched_ext contract: a struct_ops map full of callbacks.
 *   2. The three verbs: enqueue (task became runnable), dequeue (kernel
 *      takes it back), dispatch (a CPU is hungry, feed it).
 *   3. The built-in dispatch queues (DSQs): every CPU has a local DSQ
 *      (SCX_DSQ_LOCAL) it actually runs tasks from, and there is one
 *      global fallback DSQ (SCX_DSQ_GLOBAL).
 *
 * Policy implemented here: a single global FIFO. Every runnable task goes
 * to the tail of the global DSQ; idle CPUs pull from its head. That's it.
 * No fairness, no locality, no priorities - and yet the system runs.
 *
 * Talk demo: load it, run `stress-ng`, show the system stays alive, then
 * Ctrl-C and show the kernel falls back to CFS/EEVDF instantly.
 *
 * Build: drop this pair into the scx repo's scheds/c/ and add "scx_minfifo"
 * to scheds/c/meson.build — full per-distro steps (Arch, openSUSE,
 * Ubuntu 24.04) in README.md.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

/*
 * UEI = user exit info. A tiny shared structure the kernel fills in when
 * the scheduler is kicked out (error, sysrq-S, unload) so userspace can
 * report *why*. Our first example of BPF <-> userspace communication.
 */
UEI_DEFINE(uei);

/*
 * ops.enqueue - called when a task becomes runnable and the core needs us
 * to put it *somewhere*. "Somewhere" is always a DSQ.
 *
 * scx_bpf_dsq_insert(task, which_dsq, time_slice, flags):
 *   - SCX_DSQ_GLOBAL: the built-in global queue. The kernel automatically
 *     consumes it when a CPU's local DSQ runs dry, so for this toy we do
 *     not even need an ops.dispatch callback.
 *   - SCX_SLICE_DFL: default 20ms slice. When it runs out, the task is
 *     preempted and re-enqueued - which brings it right back here.
 */
void BPF_STRUCT_OPS(minfifo_enqueue, struct task_struct *p, u64 enq_flags)
{
	scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
}

/*
 * ops.dequeue - the mirror image. Called when a task leaves the BPF
 * scheduler's *custody* - and a task only enters custody when it sits in
 * a custom DSQ or in the scheduler's own data structures (BPF maps,
 * rbtrees...). Kernel 7.x formalized these semantics: tasks sent to the
 * "terminal" DSQs (LOCAL, LOCAL_ON, GLOBAL) never enter custody at all.
 *
 * This scheduler only ever touches SCX_DSQ_GLOBAL, so dequeue never
 * fires here - it is a no-op included so all three verbs are on one
 * slide. Stages 2 and 3 use custom DSQs, where the kernel does invoke
 * dequeue on property changes (affinity, nice, migration).
 */
void BPF_STRUCT_OPS(minfifo_dequeue, struct task_struct *p, u64 deq_flags)
{
}

/*
 * ops.dispatch - called when a CPU's local DSQ is empty and it wants
 * work. This is where a scheduler moves tasks from its own DSQs onto the
 * CPU's local DSQ with scx_bpf_dsq_move_to_local().
 *
 * Deliberately empty here: the kernel consumes SCX_DSQ_GLOBAL *before*
 * calling ops.dispatch, so our FIFO already flows without help. The
 * moment stage 2 switches to a custom DSQ, this callback becomes
 * load-bearing - custom DSQs are consumed by nobody but you.
 */
void BPF_STRUCT_OPS(minfifo_dispatch, s32 cpu, struct task_struct *prev)
{
}

/*
 * ops.exit - the scheduler is being unloaded (cleanly or not). Record the
 * exit info for userspace.
 */
void BPF_STRUCT_OPS(minfifo_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

/*
 * The struct_ops map itself. Every callback we did NOT set has a sane
 * default - e.g. default select_cpu picks an idle CPU near the previous
 * one. A scheduler is just: this struct, loaded.
 */
SCX_OPS_DEFINE(minfifo_ops,
	       .enqueue			= (void *)minfifo_enqueue,
	       .dequeue			= (void *)minfifo_dequeue,
	       .dispatch		= (void *)minfifo_dispatch,
	       .exit			= (void *)minfifo_exit,
	       .name			= "minfifo");
