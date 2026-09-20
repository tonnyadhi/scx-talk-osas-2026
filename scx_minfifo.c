/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_minfifo loader - Stage 1 userspace side.
 *
 * The userspace half of a sched_ext scheduler does exactly three things:
 *   1. open + load the BPF skeleton (verifier runs here),
 *   2. attach the struct_ops map (scheduling switches over here),
 *   3. wait, then report why the scheduler exited.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_minfifo.bpf.skel.h"

const char help_fmt[] =
"A minimal global-FIFO sched_ext scheduler.\n"
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

int main(int argc, char **argv)
{
	struct scx_minfifo *skel;
	struct bpf_link *link;
	__u64 ecode;
	__u32 opt;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	/* Opens the skeleton and applies scx compat handling. */
	skel = SCX_OPS_OPEN(minfifo_ops, scx_minfifo);

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

	/* Load: BPF programs are verified and pinned into the kernel. */
	SCX_OPS_LOAD(skel, minfifo_ops, scx_minfifo, uei);

	/*
	 * Attach: from this line on, every task on the system is scheduled
	 * by ~30 lines of BPF. cat /sys/kernel/sched_ext/root/ops to see it.
	 */
	link = SCX_OPS_ATTACH(skel, minfifo_ops, scx_minfifo);
	fprintf(stderr, "scx_minfifo attached. Ctrl-C to detach.\n");

	while (!exit_req && !UEI_EXITED(skel, uei))
		sleep(1);

	/* Detach: the kernel falls back to the built-in scheduler. */
	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_minfifo__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
