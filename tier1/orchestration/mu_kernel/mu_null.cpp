#include "mu/mu.hpp"

// MU profiler probes (mu/profiler/profiler.hpp) emit DeviceEvent records that
// the host-side XPTI session aggregates into Launch/Start/End/Terminate spans.
// Each null kernel brackets its (no-op) body with profilerStart/profilerEnd so
// the main_profiler binary can read meaningful device-side stage durations.
// Without these probes, DeviceEvent queries return empty.

// ── T1 / T2 / T3 / T4 / T5 / T6 / T7 / T8 ───────────────────────────────────
// Zero-work kernel: no arithmetic, no memory access beyond task descriptor.
void null_kernel(int dummy)
{
    mu::profilerStart("null_kernel");
    (void)dummy;
    mu::profilerEnd("null_kernel");
}

// ── T9: argument type sweep ───────────────────────────────────────────────────
// Each variant changes only the argument signature to isolate the command-
// parsing and GVA→SPA translation cost within the Orchestration Tax.
//
// Scalar args (ArgType::Constant) require no address translation.
// Pointer args (ArgType::DeviceMemory) each require one GVA→SPA lookup.

void null_3scalar(int a, int b, int c)
{
    mu::profilerStart("null_3scalar");
    (void)a; (void)b; (void)c;
    mu::profilerEnd("null_3scalar");
}

void null_1ptr(int* p)
{
    mu::profilerStart("null_1ptr");
    (void)p;
    mu::profilerEnd("null_1ptr");
}

void null_3ptr(int* p0, int* p1, int* p2)
{
    mu::profilerStart("null_3ptr");
    (void)p0; (void)p1; (void)p2;
    mu::profilerEnd("null_3ptr");
}

void null_6ptr(int* p0, int* p1, int* p2, int* p3, int* p4, int* p5)
{
    mu::profilerStart("null_6ptr");
    (void)p0; (void)p1; (void)p2; (void)p3; (void)p4; (void)p5;
    mu::profilerEnd("null_6ptr");
}

// ── T12: NDArray argument cost ────────────────────────────────────────────────
// NDArray is the third ArgType (Constant, DeviceMemory, NDArray). The descriptor
// is ~sizeof(NDArray<T>) (~200 B with shape/strides arrays) — much larger than
// a raw pointer (8 B). T9 measured Constant vs DeviceMemory; these isolate the
// descriptor-size component of α from the GVA→SPA lookup count.

void null_1ndarray(mu::NDArray<int> a)
{
    mu::profilerStart("null_1ndarray");
    (void)a;
    mu::profilerEnd("null_1ndarray");
}

void null_3ndarray(mu::NDArray<int> a, mu::NDArray<int> b, mu::NDArray<int> c)
{
    mu::profilerStart("null_3ndarray");
    (void)a; (void)b; (void)c;
    mu::profilerEnd("null_3ndarray");
}

// ── T15: persistent kernel amortization ───────────────────────────────────────
// Same kernel called in two patterns:
//   persistent : Map::execute(iters=N)  — kernel loops N times internally
//   repeated   : Map::execute(iters=1)  called N times — host pays α per iter
//
// The volatile sink prevents the compiler from eliding the inner loop.  The
// kernel body otherwise does nothing meaningful — the cost difference between
// the two invocation patterns is the per-launch orchestration tax α.

void persistent_noop(int iters)
{
    mu::profilerStart("persistent_noop");
    volatile int sink = 0;
    for (int i = 0; i < iters; i++)
    {
        sink += i;
    }
    (void)sink;
    mu::profilerEnd("persistent_noop");
}

MU_KERNEL_ADD(null_kernel);
MU_KERNEL_ADD(null_3scalar);
MU_KERNEL_ADD(null_1ptr);
MU_KERNEL_ADD(null_3ptr);
MU_KERNEL_ADD(null_6ptr);
MU_KERNEL_ADD(null_1ndarray);
MU_KERNEL_ADD(null_3ndarray);
MU_KERNEL_ADD(persistent_noop);
