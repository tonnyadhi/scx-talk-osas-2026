/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_toydsq loader - Stage 2 userspace side.
 *
 * New over stage 1: reading a per-CPU BPF map every second. This is the
 * simplest form of the BPF <-> userspace channel: the scheduler counts,
 * userspace aggregates and prints.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_toydsq.bpf.skel.h"

const char help_fmt[] =
"A toy vtime-fair sched_ext scheduler with one shared custom DSQ.\n"
"\n"
"Usage: %s [-v]\n"
"\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int sig)
{
	exit_req = 1;
}

/*
 * A per-CPU array map holds one value *per CPU* per key; sum them to get
 * the global counter. Lock-free on the BPF side, one syscall here.
 */
static void read_stats(struct scx_toydsq *skel, __u64 *stats)
{
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 cnts[2][nr_cpus];
	__u32 idx;

	memset(stats, 0, sizeof(stats[0]) * 2);

	for (idx = 0; idx < 2; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[idx][cpu];
	}
}

int main(int argc, char **argv)
{
	struct scx_toydsq *skel;
	struct bpf_link *link;
	__u64 ecode;
	__u32 opt;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	skel = SCX_OPS_OPEN(toydsq_ops, scx_toydsq);

	while ((opt = getopt(argc, argv, "vh")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	SCX_OPS_LOAD(skel, toydsq_ops, scx_toydsq, uei);
	link = SCX_OPS_ATTACH(skel, toydsq_ops, scx_toydsq);
	fprintf(stderr, "scx_toydsq attached. Ctrl-C to detach.\n");

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[2];

		read_stats(skel, stats);
		/*
		 * "direct" should dominate on an idle-ish machine (wakeups
		 * find idle CPUs); load the machine and watch "shared"
		 * climb as tasks start queueing.
		 */
		printf("direct=%llu shared=%llu\n", stats[0], stats[1]);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_toydsq__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
