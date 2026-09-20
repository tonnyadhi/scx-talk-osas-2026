# sched_ext demo schedulers — openSUSE Asia 2026

Three schedulers, one per act of the talk. Each stage introduces exactly one or two new concepts on top of the previous one.

| Stage | Scheduler | New concepts | BPF LoC (approx) |
|---|---|---|---|
| 1 | `scx_minfifo` | struct_ops, enqueue/dequeue/dispatch, built-in DSQs (global + per-CPU local) | ~40 |
| 2 | `scx_toydsq` | custom DSQ, `select_cpu` idle fast path, vtime priority queue (weighted fairness), stats via percpu map | ~110 |
| 3 | `scx_prefcore` | AMD preferred-core ranking via sysfs → rodata, task local storage, online latency/batch classification, per-tier DSQ consumption, cpuperf sampling | ~200 |

## Requirements

- Kernel ≥ 6.12 with `CONFIG_SCHED_CLASS_EXT=y` — see the per-distro notes below. **7.0+ recommended**: that is where the `scx_bpf_*` kfunc surface stabilized (plus EEVDF-interaction fixes), and the kfunc names and signatures used in this code match the 7.x documented API exactly.
- Stage 2 uses `scx_bpf_dsq_insert_vtime`; current kfunc spellings are used throughout (`scx_bpf_dsq_insert`, `scx_bpf_dsq_move_to_local(dsq_id, enq_flags)`). The scx repo compat headers paper over older kernels.
- clang ≥ 16, meson, libbpf ≥ 1.4 (scx's meson setup fetches and builds its own libbpf/bpftool if the system one is missing or too old, depending on scx version — network required in that case).
- For the full stage-3 story: an AMD machine exposing `/sys/devices/system/cpu/cpu*/cpufreq/amd_pstate_prefcore_ranking` (amd-pstate in active/guided mode with preferred-core support). The loader falls back to `acpi_cppc/highest_perf`, then `cpuinfo_max_freq`, then uniform ranks — so it loads anywhere, it is just less interesting.

## Step 0 — verify your kernel supports sched_ext

Same check everywhere; do this before installing anything:

```sh
# Either of these confirms support:
zgrep CONFIG_SCHED_CLASS_EXT /proc/config.gz            # distros with /proc/config.gz (Arch, openSUSE)
grep CONFIG_SCHED_CLASS_EXT /boot/config-$(uname -r)    # distros with /boot configs (Ubuntu)
ls /sys/kernel/sched_ext/                               # exists only on sched_ext-capable kernels
```

You want `CONFIG_SCHED_CLASS_EXT=y` and a kernel ≥ 6.12 (`uname -r`).

## Step 1 — install build dependencies (per distro)

### Arch Linux

Stock `linux` kernel ≥ 6.12 ships with sched_ext enabled — no kernel work needed.

```sh
sudo pacman -S --needed base-devel git meson clang llvm libbpf libelf zlib pkgconf
```

Optional: `sudo pacman -S scx-scheds` installs the prebuilt reference schedulers
(`scx_simple`, `scx_lavd`, ...) — handy for comparing against your toys, not needed for building them.

### openSUSE Tumbleweed / Slowroll

Stock kernel ≥ 6.12 ships with sched_ext enabled. (Leap 15.x kernels are too old — use Tumbleweed for the demo box.)

```sh
sudo zypper install git meson clang llvm gcc pkg-config libbpf-devel libelf-devel zlib-devel
```

Tumbleweed also packages the reference schedulers as `scx`, same caveat as Arch.

### Ubuntu 24.04 LTS (noble)

The GA kernel is 6.8 — **no sched_ext**. You need the HWE kernel (6.14 as of 24.04.3) and must verify the config flag after rebooting into it:

```sh
sudo apt install linux-generic-hwe-24.04
sudo reboot
grep CONFIG_SCHED_CLASS_EXT /boot/config-$(uname -r)   # must print =y
```

If your HWE kernel does not set the flag, options are a newer Ubuntu release (24.10+ kernels have it enabled; releases shipping 7.x kernels are the best fit — see Requirements) or a mainline/custom kernel with `CONFIG_SCHED_CLASS_EXT=y`. On any Ubuntu kernel, 7.x included, keep the config check — it is the ground truth. Then:

```sh
sudo apt install build-essential git meson clang llvm pkg-config \
                 libbpf-dev libelf-dev zlib1g-dev
```

Noble's clang is 18 (fine). Noble's libbpf is 1.3; if meson complains it is too old, let scx's build fetch its own copy (it will say so), or build libbpf from source first.

## Step 2 — build (scx repo drop-in)

Identical on all three distros:

```sh
git clone https://github.com/sched-ext/scx.git
cd scx
cp /path/to/scx-talk-demos/scx_*.c scheds/c/
cp /path/to/scx-talk-demos/scx_*.bpf.c scheds/c/
```

Edit `scheds/c/meson.build` and add the three names to the scheduler list (next to `scx_simple`):

```meson
c_scheds = ['scx_simple', 'scx_qmap', ..., 'scx_minfifo', 'scx_toydsq', 'scx_prefcore']
```

Then:

```sh
meson setup build --prefix ~
meson compile -C build
```

Binaries land in `build/scheds/c/`. To rebuild after editing a `.bpf.c`, just rerun `meson compile -C build`.

### Alternative: manual build with clang/gcc only (no meson)

`Makefile.manual` in this directory drives the four raw steps meson normally hides. You still need a scx checkout for the compat headers, but nothing is built from it:

```sh
# extra tool: bpftool (Arch: bpftool | openSUSE: bpftool
#                      | Ubuntu: linux-tools-common linux-tools-$(uname -r))
git clone https://github.com/sched-ext/scx.git ~/src/scx     # headers only
cd scx-talk-demos
make -f Makefile.manual SCX=$HOME/src/scx info    # check what resolved
make -f Makefile.manual SCX=$HOME/src/scx
sudo ./scx_minfifo
```

**Always run `info` first.** It prints the four things that decide whether the build works:

```
SCX_VMLINUX = .../scx/scheds/vmlinux/arch/x86/vmlinux.h   # scx's, not the kernel's
MULTIARCH   = x86_64-linux-gnu                            # Ubuntu/Debian only
LIBBPF      = <system -lbpf>                              # or a path to libbpf.a
```

On **Arch / openSUSE** the defaults are correct and `make -f Makefile.manual SCX=...` is all you need.

On **Ubuntu 24.04** the system libbpf (1.3) is too old for struct_ops skeletons, so build one:

```sh
git clone https://github.com/libbpf/libbpf ~/src/libbpf
cd ~/src/libbpf/src && make BUILD_STATIC_ONLY=y DESTDIR=$PWD/root install
cd -
make -f Makefile.manual SCX=$HOME/src/scx LIBBPF=$HOME/src/libbpf/src/root
```

Use `$HOME/...` or a `./relative` path — the shell does **not** expand `~..`, and a wrong `LIBBPF` now aborts the build rather than silently using the system one. (Ubuntu's easier path remains meson, which vendors its own libbpf and bpftool.)

What each step is, if you want to run them by hand:

```sh
# 1. dump the running kernel's types into a header (CO-RE keeps it portable)
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# 2. compile the BPF side — this step is clang-only (gcc's BPF backend
#    cannot build sched_ext programs); -g is mandatory, it emits the BTF
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -mcpu=v3 \
      -I. -I ~/src/scx/scheds/include -c scx_minfifo.bpf.c -o scx_minfifo.bpf.o

# 3. wrap the object into a skeleton header ("name" must be scx_minfifo —
#    it becomes the struct/type prefix the loader uses)
bpftool gen skeleton scx_minfifo.bpf.o name scx_minfifo > scx_minfifo.bpf.skel.h

# 4. build the loader — plain C, gcc or clang both fine
gcc -O2 -I. -I ~/src/scx/scheds/include scx_minfifo.c -o scx_minfifo -lbpf -lelf -lz
```

This is also excellent talk material: it demystifies what "the build system" does — one type dump, one clang invocation, one code generator, one ordinary link.

#### Troubleshooting the manual build

**`fatal error: 'scx/common.bpf.h' file not found`** — `SCX` is pointing at the wrong place. It must be the path to an **scx checkout**, not to this demo directory:

```sh
make -f Makefile.manual SCX=.              # wrong: there is no ./scheds/include here
git clone https://github.com/sched-ext/scx.git ~/src/scx
make -f Makefile.manual SCX=~/src/scx      # right
```

Nothing is compiled from the checkout — only `scheds/include/scx/*.h` is borrowed. The Makefile now checks for `$(SCX)/scheds/include/scx/common.bpf.h` up front and prints this hint instead of a clang error. If a future scx release relocates the headers, point at them directly: `make -f Makefile.manual SCX_INC=/path/to/include`.

**`struct bpf_map_skeleton has no member named 'link'` (Ubuntu 24.04)** — your `bpftool` is newer than your `libbpf-dev`. Noble ships libbpf 1.3; struct_ops skeletons need 1.4+, so bpftool emits `map->link` and the old headers reject it. Build a current libbpf and point the Makefile at it:

```sh
git clone https://github.com/libbpf/libbpf ~/src/libbpf
cd ~/src/libbpf/src && make BUILD_STATIC_ONLY=y DESTDIR=$PWD/root install
cd -   # back to scx-talk-demos
make -f Makefile.manual clean
make -f Makefile.manual SCX=../scx LIBBPF=~/src/libbpf/src/root
```

`make info` will confirm: `LIBBPF = .../libbpf.a` instead of `<system -lbpf>`. Static linking also means the binaries you demo carry no runtime library-path surprises. (Alternative: use the meson path on Ubuntu — scx's build can vendor its own libbpf and bpftool. The manual Makefile is the teaching artifact; meson is the path of least resistance on older distros.)

**`struct ... __sched_ext_ops` has no member named `rescue_bandwidth_ppt` / `sub_attach` / `sub_detach` / `sub_cgroup_id`** (or any other field, inside `SCX_OPS_OPEN`) — **you compiled against a `vmlinux.h` generated from your running kernel instead of the one bundled in the scx repo.** The BPF side compiling cleanly while only the loader fails is the signature. The `sub_*` fields are sub-scheduler support (`CONFIG_EXT_SUB_SCHED`), absent from kernels built without it — another reason not to depend on the local BTF.

Why it happens: `scx/compat.h` is written against recent kernel types. It references newer `sched_ext_ops` fields at *compile* time and zeroes them at *runtime* when the live kernel lacks them — that is the whole point of the compat layer. So the correct input is scx's own newer `vmlinux.h`; CO-RE relocations make the result load fine on your older kernel.

The Makefile now auto-detects scx's bundled header. Confirm what it resolved to:

```sh
make -f Makefile.manual SCX=../scx info
#   SCX_VMLINUX = ../scx/scheds/include/arch/x86/vmlinux.h    <- good
#   SCX_VMLINUX = <none found ...>                            <- see below
make -f Makefile.manual clean && make -f Makefile.manual SCX=../scx
```

If auto-detection finds nothing, locate the header and point at it explicitly:

```sh
find ../scx -name 'vmlinux*.h' | head
make -f Makefile.manual SCX=../scx SCX_VMLINUX=../scx/path/to/vmlinux.h
```

Pinning scx to an older revision also resolves it, and is the right move if you want the checkout to match your kernel exactly:

```sh
cd ../scx && git log --oneline -S rescue_bandwidth_ppt -- scheds/include/scx/compat.h | tail -1
git checkout <that_commit>^
```

**`fatal error: 'bits/wordsize.h' file not found` (Debian/Ubuntu only)** — scx's `compat.bpf.h` includes `<errno.h>`, which reaches `bits/wordsize.h`. On Debian/Ubuntu that header lives under the multiarch directory `/usr/include/x86_64-linux-gnu/bits/`, and `clang -target bpf` does not add it automatically (changing the target triple drops the host's default include paths). Arch and openSUSE keep it in `/usr/include/bits/`, so they never hit this.

The Makefile now detects the multiarch dir via `cc -print-multiarch` and appends it. Confirm with:

```sh
make -f Makefile.manual SCX=../scx info
#   MULTIARCH   = x86_64-linux-gnu        <- Ubuntu/Debian, dir will be added
#   MULTIARCH   = <none - non-multiarch>  <- Arch/openSUSE, nothing needed
```

Manual equivalent if you are running clang by hand: add `-I/usr/include/$(cc -print-multiarch)`.

**`redefinition of enumerator 'NR_STATS'` (or any identifier, pointing at vmlinux.h)** — your name collides with a kernel identifier. vmlinux.h is the *entire kernel's* type universe dumped into one header, and BPF C shares one namespace with it, so short generic names (`NR_STATS`, `MAX_ENTRIES`, `STAT_READ`...) are land mines. Prefix your enums and structs — this repo uses `PREFCORE_NR_STATS` for exactly this reason. Worth ten seconds in the talk: it surprises everyone once.

**`declaration does not declare anything [-Wmissing-declarations]` (×9 or so, from vmlinux.h)** — harmless. bpftool's type dump emits forward declarations for anonymous types. The Makefile passes `-Wno-missing-declarations` to keep the build output quiet on stage.

**`skipping /sys/kernel/btf/vmlinux (will be loaded as base)` on stderr** — harmless bpftool chatter, not an error; the dump still runs. Sanity check with `wc -l vmlinux.h` (expect tens of thousands of lines).

**`bpftool: command not found`** — Arch: `pacman -S bpf`; openSUSE: `zypper in bpftool`; Ubuntu: `apt install linux-tools-common linux-tools-$(uname -r)`.

**`fatal error: 'bpf/bpf_helpers.h' file not found`** — libbpf headers missing: `libbpf` (Arch) / `libbpf-devel` (openSUSE) / `libbpf-dev` (Ubuntu).

**`cannot find -lbpf` at link time** — the loader links against system libbpf; on Ubuntu 24.04 the packaged libbpf is 1.3, which is usually fine for these loaders. If it is not, build libbpf from source or fall back to the meson path, which can vendor its own.

**Before the talk, diff against the current `scheds/c/scx_simple.c`/`.bpf.c` in your checkout.** The sched_ext API still moves; the compat macros (`SCX_OPS_OPEN/LOAD/ATTACH`, `UEI_*`) and kfunc names in these files match early-2026 scx, but the repo is the source of truth. See "API checkpoints" below.

## Step 3 — load, verify, unload

```sh
sudo ./build/scheds/c/scx_minfifo          # attach (root needed: it loads BPF + struct_ops)
```

In a second terminal, verify it is really scheduling the machine:

```sh
cat /sys/kernel/sched_ext/state            # "enabled"
cat /sys/kernel/sched_ext/root/ops         # "minfifo"
sudo bpftool struct_ops list               # optional: shows the registered ops map
```

Unload paths, from polite to emergency:

1. `Ctrl-C` the loader — detaches, kernel falls back to EEVDF instantly.
2. The watchdog — if your scheduler starves a runnable task past the timeout, the kernel ejects it on its own and `state` returns to `disabled`.
3. `sudo sh -c 'echo S > /proc/sysrq-trigger'` — manual ejector seat (sysrq-S), kicks out whatever BPF scheduler is loaded. Check `dmesg | tail` afterwards to see the exit reason.

Run stages 2 and 3 the same way (`scx_toydsq`, `scx_prefcore -s 4 -t 1000`); their loaders print per-second stats to stdout.

## Appendix — enabling BPF + sched_ext in a custom kernel

Only needed when no distro kernel qualifies (e.g. Ubuntu without a suitable HWE kernel, or you want to run mainline). The config snippet:

```text
# --- BPF core ---
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_JIT_DEFAULT_ON=y

# --- BTF type info: CO-RE and struct_ops depend on this ---
# (build-time requirement: pahole >= 1.25, from the "dwarves" package)
CONFIG_DEBUG_INFO=y
CONFIG_DEBUG_INFO_BTF=y

# --- the extensible scheduler class itself ---
CONFIG_SCHED_CLASS_EXT=y

# --- nice to have for debugging ---
CONFIG_SCHED_DEBUG=y
```

Do **not** set `CONFIG_PAHOLE_HAS_*` by hand — Kconfig derives those automatically from the pahole version it finds at build time; if they come out `n`, upgrade `dwarves` and reconfigure.

Build steps, starting from your running kernel's config:

```sh
# pahole first: Arch: pacman -S pahole | openSUSE: zypper in dwarves | Ubuntu: apt install dwarves
cd linux/                                       # source >= 6.12
zcat /proc/config.gz > .config 2>/dev/null \
  || cp /boot/config-$(uname -r) .config        # inherit current config
scripts/config -e BPF -e BPF_SYSCALL -e BPF_JIT -e BPF_JIT_ALWAYS_ON \
               -e BPF_JIT_DEFAULT_ON -e DEBUG_INFO -e DEBUG_INFO_BTF \
               -e SCHED_CLASS_EXT -e SCHED_DEBUG
make olddefconfig                               # resolve new dependencies
make -j$(nproc)
sudo make modules_install install
sudo reboot                                     # then re-run the Step 0 check
```

After reboot, `ls /sys/kernel/sched_ext/` existing confirms the class is in; `bpftool btf dump file /sys/kernel/btf/vmlinux format c | head` confirms BTF is present.

## Step 4 — monitor the scheduler while it runs

Three layers of observability, from "is it in charge" to "what does it cost":

**Kernel state (sysfs).** Whether sched_ext is active and which scheduler owns the machine:

```sh
watch -n1 'cat /sys/kernel/sched_ext/state /sys/kernel/sched_ext/root/ops'
# state: enabled | disabled     ops: minfifo / toydsq / prefcore
cat /sys/kernel/sched_ext/enable_seq   # how many BPF schedulers loaded since boot
```

**Kernel-side event counters (7.x kernels).** The kernel itself keeps per-scheduler diagnostic counters — no code on your side needed:

```sh
cat /sys/kernel/sched_ext/root/events
# SCX_EV_SELECT_CPU_FALLBACK: your select_cpu returned a CPU the task can't use
# SCX_EV_DISPATCH_KEEP_LAST:  CPU kept running the same task (nothing else queued)
# SCX_EV_REFILL_SLICE_DFL:    slices refilled with the default value
# SCX_EV_BYPASS_*:            time spent in bypass (load/unload/error recovery)
```

`SCX_EV_SELECT_CPU_FALLBACK` climbing under scx_prefcore would mean the ranked scan is returning disallowed CPUs — a free correctness check on the demo box. `tools/sched_ext/scx_show_state.py` in the kernel tree (a drgn script) shows even more.

**Your own counters (the loaders' stdout).** This is the maps channel doing its job:

- `scx_toydsq`: `direct=` (wakeups that found an idle CPU) vs `shared=` (queued). Idle machine → direct dominates; under `stress-ng` → shared climbs.
- `scx_prefcore`: `direct/lat/batch/xtier` plus the live cpuperf level of the best and worst core. `xtier` counts cross-tier rescues — near zero means the tiers are well sized.

**Placement, visually.** `htop` or `btop` with per-core meters: start `stress-ng --cpu $(nproc)` under `scx_prefcore` and watch load settle on the weak cores while the strong cores stay responsive. `scxtop` (ships with the scx repo / distro scx packages) gives a top-style live view of DSQs and scheduler statistics.

**What the scheduler itself costs (bpftool).** Enable BPF runtime stats and read per-callback counts:

```sh
sudo bpftool struct_ops list                  # the registered scheduler ops
sudo sysctl kernel.bpf_stats_enabled=1
sudo bpftool prog show | grep -B1 -A2 enqueue
#   run_cnt 184223  run_time_ns ...           # how often + how much CPU each callback used
sudo sysctl kernel.bpf_stats_enabled=0        # turn accounting back off
```

Good talk beat: hundreds of thousands of callback invocations, total cost in milliseconds.

**Deep inspection / stuck states (SysRq).**

```sh
sudo sh -c 'echo D > /proc/sysrq-trigger'     # dump full scheduler state (DSQs, runnable tasks) to dmesg
sudo dmesg | tail -80
sudo sh -c 'echo S > /proc/sysrq-trigger'     # eject the BPF scheduler entirely
```

The `D` dump is also what you get automatically when the watchdog kills a misbehaving scheduler — reading one on a healthy system makes the crash-day version much less scary.

## Demo flow

### Stage 1 — `scx_minfifo`

1. `sudo ./scx_minfifo` — the whole machine is now scheduled by ~40 lines of BPF.
2. Show it is live: `cat /sys/kernel/sched_ext/root/ops` → `minfifo`; `cat /sys/kernel/sched_ext/state` → `enabled`.
3. `stress-ng --cpu $(nproc) --timeout 60` in another terminal: desktop stays usable-ish. Point out there is *no fairness code at all* — just FIFO order and slice expiry.
4. Ctrl-C → instant fallback to EEVDF. Nothing to reboot.
5. Safety slide material: break the scheduler on purpose (e.g. comment out the dispatch *and* enqueue into a custom DSQ nobody consumes) → the runnable-task watchdog aborts the scheduler after `timeout_ms` and the kernel falls back. `sysrq-S` is the manual ejector seat.

### Stage 2 — `scx_toydsq`

1. `sudo ./scx_toydsq` — prints `direct=… shared=…` every second.
2. Idle machine: `direct` dominates (wakeups find idle CPUs and skip the queue). Under `stress-ng`: `shared` climbs — queueing begins, and *now* the vtime ordering matters.
3. Fairness demo: run two CPU hogs, one at `nice -n 0`, one at `nice -n 10`; show CPU split follows the weights in `htop`. Then point at the one line in `stopping()` that implements it.

### Stage 3 — `scx_prefcore`

1. Show the raw ranking first: `grep . /sys/devices/system/cpu/cpu*/cpufreq/amd_pstate_prefcore_ranking`.
2. `sudo ./scx_prefcore` — prints the ranking table (cpu, rank, strong/weak tier), then per-second stats including the cpuperf level of the best- and worst-ranked CPU.
3. Load the weak tier: `stress-ng --cpu $(nproc)` → batch stress threads drift to weak cores (watch per-CPU bars in `btop`/`htop`), while something interactive (terminal, a `schbench`/`hackbench -l` run, audio) stays snappy on the strong cores.
4. Show classification is *online*: a stress thread starts as latency class (fresh EWMA = 0) and migrates to batch within a few slices.
5. Tuning knobs live in userspace: `-s 4` (strong-tier size), `-t 500` (classification threshold in µs) — restart, no recompilation of policy, no reboot, ever.

## How the pieces map to the abstract

- "dispatch queues, the enqueue/dispatch callbacks" → stage 1, entire file.
- "how a BPF scheduler talks to userspace through maps" → stage 2 (percpu stats map, read side in the loader) and stage 3 (rodata as load-time config: sysfs ranking → `cpu_order[]`/`strong_cpu[]`; task local storage; cpuperf sample map).
- "reads AMD preferred-core ranking and steers latency-sensitive tasks onto the strongest cores" → stage 3 `select_cpu` (ranked scan direction per class) + `dispatch` (per-tier DSQ preference with cross-tier fallback for work conservation).
- "using the kernel's cpuperf interface" → stage 3 `tick()` samples `scx_bpf_cpuperf_cur()`; mention `scx_bpf_cpuperf_set()` as the write side.

## API checkpoints (verify at build time)

**Check arity before you build** — names drift, but so do argument lists. One command tells you what your checkout expects:

```sh
grep -n -E 'define (scx_bpf_dsq_insert|scx_bpf_dsq_insert_vtime|scx_bpf_dsq_move_to_local|scx_bpf_select_cpu_dfl)' \
     ../scx/scheds/include/scx/*.bpf.h
# ...and the canonical usage, which is always the ground truth:
grep -n 'scx_bpf_dsq_' ../scx/scheds/c/scx_simple.bpf.c
```

Known moving target: **`scx_bpf_dsq_move_to_local`** takes `(dsq_id, enq_flags)` in current scx; older revisions take `(dsq_id)` only, and older still call it `scx_bpf_consume(dsq_id)`. The code here uses the two-argument form with `enq_flags = 0`. If your checkout errors with *"too many arguments provided"*, drop the `, 0` in `scx_toydsq.bpf.c` and `scx_prefcore.bpf.c`.

Names most likely to have drifted; all are used here with their current (kernel 7.x-documented) spellings:

- `scx_bpf_dsq_insert`, `scx_bpf_dsq_insert_vtime` (formerly `scx_bpf_dispatch*`)
- `scx_bpf_dsq_move_to_local` (formerly `scx_bpf_consume`; gained an `enq_flags` argument)

Semantics worth knowing on 7.x kernels: `ops.dequeue` follows formal "custody" rules — it fires only when a task leaves a *custom* DSQ or the scheduler's own data structures. Tasks sent to terminal DSQs (`LOCAL`, `LOCAL_ON`, `GLOBAL`) never enter custody, so scx_minfifo's dequeue never runs, while toydsq/prefcore (custom DSQs) do get dequeue calls on affinity/priority changes. The kernel's ABI-instability warning still stands regardless of version; the 7.0 stabilization is a practice, not a promise.
- `scx_bpf_select_cpu_dfl`, `scx_bpf_test_and_clear_cpu_idle`, `scx_bpf_create_dsq`
- `scx_bpf_cpuperf_cur`, `scx_bpf_cpuperf_cap`, `SCX_CPUPERF_ONE`
- Macros from scx compat headers: `SCX_OPS_DEFINE`, `SCX_OPS_OPEN`, `SCX_OPS_LOAD`, `SCX_OPS_ATTACH`, `UEI_DEFINE/RECORD/REPORT/EXITED`, `BPF_STRUCT_OPS(_SLEEPABLE)`

## Honest limitations (good closing slide)

- No NUMA/LLC awareness; ranked scan ignores cache topology entirely.
- No kick-on-enqueue: a task enqueued while its preferred tier idles waits for the next dispatch event. Production schedulers use `scx_bpf_kick_cpu()`.
- Classification by slice consumption is crude — a video decoder and a compiler can look alike. `scx_lavd` shows the serious version.
- Strong/weak is a static median split of a ranking that the firmware can change at runtime (`amd_pstate_prefcore_ranking` is dynamic on some parts); we read it once at load.
