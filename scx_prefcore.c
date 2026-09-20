/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_prefcore loader - Stage 3 userspace side.
 *
 * New over stage 2: configuring the BPF program *before* it loads.
 * Between skeleton open and load we can write skel->rodata; those values
 * are frozen at load time and the verifier treats them as constants.
 *
 * Ranking sources, in order of preference:
 *   1. /sys/devices/system/cpu/cpuN/cpufreq/amd_pstate_prefcore_ranking
 *      (amd-pstate with preferred-core support)
 *   2. /sys/devices/system/cpu/cpuN/acpi_cppc/highest_perf
 *   3. /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq
 * On a machine with uniform cores every rank ties and the scheduler
 * degrades to plain two-class scheduling - still loadable for the demo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_prefcore.bpf.skel.h"

#define MAX_CPUS 512

const char help_fmt[] =
"A toy sched_ext scheduler aware of AMD preferred-core ranking.\n"
"\n"
"Usage: %s [-s STRONG] [-t THRESH_US] [-v]\n"
"\n"
"  -s STRONG     Number of CPUs in the strong tier (default: nr_cpus/2)\n"
"  -t THRESH_US  Latency-class threshold in usec (default: 1000)\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

static bool verbose;
static volatile int exit_req;

struct cpu_rank {
	int cpu;
	long rank;
};

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

static int read_long_file(const char *path, long *val)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (fscanf(f, "%ld", val) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static const char *ranking_files[] = {
	"/sys/devices/system/cpu/cpu%d/cpufreq/amd_pstate_prefcore_ranking",
	"/sys/devices/system/cpu/cpu%d/acpi_cppc/highest_perf",
	"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
};

static const char *read_rankings(struct cpu_rank *ranks, int nr_cpus)
{
	char path[256];
	size_t src;
	int cpu;

	for (src = 0; src < sizeof(ranking_files) / sizeof(ranking_files[0]);
	     src++) {
		bool ok = true;

		for (cpu = 0; cpu < nr_cpus; cpu++) {
			snprintf(path, sizeof(path), ranking_files[src], cpu);
			ranks[cpu].cpu = cpu;
			if (read_long_file(path, &ranks[cpu].rank)) {
				ok = false;
				break;
			}
		}
		if (ok)
			return ranking_files[src];
	}

	/* No source available: uniform ranks, natural CPU order. */
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		ranks[cpu].cpu = cpu;
		ranks[cpu].rank = 0;
	}
	return "(none - uniform)";
}

/* Sort best rank first; ties keep natural CPU order (qsort is not stable,
 * so break ties on cpu id explicitly). */
static int cmp_rank(const void *a, const void *b)
{
	const struct cpu_rank *ra = a, *rb = b;

	if (rb->rank != ra->rank)
		return rb->rank > ra->rank ? 1 : -1;
	return ra->cpu - rb->cpu;
}

int main(int argc, char **argv)
{
	struct cpu_rank ranks[MAX_CPUS];
	struct scx_prefcore *skel;
	struct bpf_link *link;
	const char *src;
	int nr_cpus, nr_strong = 0, i;
	long thresh_us = 1000;
	__u64 ecode;
	__u32 opt;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	nr_cpus = libbpf_num_possible_cpus();
	if (nr_cpus < 0 || nr_cpus > MAX_CPUS) {
		fprintf(stderr, "unsupported nr_cpus %d\n", nr_cpus);
		return 1;
	}
restart:
	skel = SCX_OPS_OPEN(prefcore_ops, scx_prefcore);

	while ((opt = getopt(argc, argv, "s:t:vh")) != -1) {
		switch (opt) {
		case 's':
			nr_strong = atoi(optarg);
			break;
		case 't':
			thresh_us = atol(optarg);
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}
	if (nr_strong <= 0 || nr_strong > nr_cpus)
		nr_strong = nr_cpus / 2 > 0 ? nr_cpus / 2 : 1;

	src = read_rankings(ranks, nr_cpus);
	qsort(ranks, nr_cpus, sizeof(ranks[0]), cmp_rank);

	/*
	 * Configure the BPF side. This must happen after open and before
	 * load: rodata is frozen (and verified) at load time.
	 */
	skel->rodata->nr_cpus = nr_cpus;
	skel->rodata->lat_thresh_ns = thresh_us * 1000;
	for (i = 0; i < nr_cpus; i++) {
		skel->rodata->cpu_order[i] = ranks[i].cpu;
		skel->rodata->strong_cpu[ranks[i].cpu] = i < nr_strong;
	}

	printf("ranking source: %s\n", src);
	printf("%-6s %-8s %s\n", "cpu", "rank", "tier");
	for (i = 0; i < nr_cpus; i++)
		printf("cpu%-3d %-8ld %s\n", ranks[i].cpu, ranks[i].rank,
		       i < nr_strong ? "strong" : "weak");

	SCX_OPS_LOAD(skel, prefcore_ops, scx_prefcore, uei);
	link = SCX_OPS_ATTACH(skel, prefcore_ops, scx_prefcore);
	fprintf(stderr, "scx_prefcore attached. Ctrl-C to detach.\n");

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 totals[4] = {};
		__u64 cnts[MAX_CPUS];
		__u32 idx, best, worst;
		__u32 perf_best = 0, perf_worst = 0;
		int cpu;

		for (idx = 0; idx < 4; idx++) {
			if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
						&idx, cnts) < 0)
				continue;
			for (cpu = 0; cpu < nr_cpus; cpu++)
				totals[idx] += cnts[cpu];
		}

		/* cpuperf level of the best- and worst-ranked CPU */
		best = ranks[0].cpu;
		worst = ranks[nr_cpus - 1].cpu;
		bpf_map_lookup_elem(bpf_map__fd(skel->maps.cpuperf_sample),
				    &best, &perf_best);
		bpf_map_lookup_elem(bpf_map__fd(skel->maps.cpuperf_sample),
				    &worst, &perf_worst);

		printf("direct=%llu lat=%llu batch=%llu xtier=%llu | "
		       "cpuperf cpu%u=%u cpu%u=%u (of 1024)\n",
		       totals[0], totals[1], totals[2], totals[3],
		       best, perf_best, worst, perf_worst);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_prefcore__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
