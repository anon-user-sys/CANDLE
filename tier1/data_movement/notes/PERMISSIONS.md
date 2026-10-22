# Running the suite — required privileges

**TL;DR: every binary in `data_movement_tax/` must be run as root (or with a
udev rule granting access to the CXL control devices), otherwise `libpxl`
silently fails to map device memory into the user process and the first
host-side touch — `pxl::flushHostCache`, `memcpy(devPtr, …)`, or a direct
indexed store — faults with SIGBUS.**

```bash
sudo ./main d1     # works
./main d1          # SIGBUS
```

## Why

Two CXL control devices are root-only on this server:

```
/dev/cxl/mem0   crw-------  root:root   ← needs CAP_SYS_ADMIN or root
/dev/ndctl0     crw-------  root:root
```

The DMA channels are world-writable:

```
/dev/mx_dma/mx_dma0_*   crw-rw-rw-  root:root
```

So when a non-privileged user calls `pxl::runtime::Context::memAlloc()`:

1. The driver allocates device memory.
2. It attempts to `mmap` the region into the user process through
   `/dev/cxl/mem0`. **This step fails silently** — the user doesn't get
   an error from `memAlloc`; it returns a non-NULL pointer to an
   address in the reserved range that isn't actually backed.
3. Any host-CPU operation on that pointer (`clflushopt`, load, store,
   SIMD) faults with SIGBUS because the address isn't in the process's
   page tables.
4. The `copyToDevice` / `copyFromDevice` DMA paths bypass the user
   mapping entirely (they go through `/dev/mx_dma/*`), so those calls
   succeed — which is what made the issue confusing: kernel dispatch
   works, but host coherence ops don't.

This was diagnosed by:
- `/proc/self/maps` shows no entry covering the `memAlloc` return value.
- `gdb` on `memory_test` shows SIGBUS at `clflushopt (%rax)` inside
  `pxl::flushHostCache`.
- Running the same binary under `sudo` succeeds at 14.8 GiB/s.

## Permanent fix (no sudo)

Add a udev rule that grants a chosen group access to the two control
devices:

```
# /etc/udev/rules.d/99-cxl-mx.rules
SUBSYSTEM=="cxl", KERNEL=="mem*", MODE="0660", GROUP="cxl"
SUBSYSTEM=="nd",  KERNEL=="ndctl*", MODE="0660", GROUP="cxl"
```

Then create the group and add your user:

```bash
sudo groupadd cxl
sudo usermod -aG cxl $USER
sudo udevadm control --reload-rules
sudo udevadm trigger
# log out and back in
```

After that, all `data_movement_tax` binaries (and the shipped XCENA SDK
examples) run without sudo.

## What this means for the benchmark results in the paper

Numbers reported by this suite are taken under sudo. They should be
identical to numbers a system admin would see — the kernel driver doesn't
do anything different for root vs an authorized non-root user, the
permission check is purely on the device-file open. Mention this in the
methodology section but don't treat it as a confounder.

## What this is NOT

This is **not**:
- A regression in libpxl (the library is 2026-03-16, current).
- A bug in the SDK tutorials (they assume the standard install where users
  have the right group membership).
- An issue with `memAlloc` semantics (the documented contract is fine; the
  failure is a silent-mmap-fail in the driver path).

This **is** a UX papercut worth reporting upstream: `memAlloc` should
return `nullptr` (or `pxl::PxlResult::Failure`) instead of a non-NULL
pointer that crashes on first use. We left a note in the original report
(see git history of CRASH_REPORT.md prior to this rewrite).
