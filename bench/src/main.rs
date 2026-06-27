use ndarray::Array2;
use std::hint::black_box;
use std::time::Instant;

/// Idiomatic ndarray usage: Zip handles its own internal pointer-stepping,
/// no manual indexing, no manual loop. This is the "control" -- well
/// optimized ndarray code shouldn't need our passes to help much, since
/// none of the anti-patterns they target (recomputed stride multiplies,
/// bounds-checked indexing, escaping-but-shouldn't-be heap buffers, inlined
/// sync/io overhead) are present here in the first place.
#[inline(never)]
pub fn zip_clean(a: &Array2<f32>, b: &Array2<f32>, out: &mut Array2<f32>) {
    ndarray::Zip::from(a).and(b).and(out).for_each(|&x, &y, o| {
        *o = x + y;
    });
}

/// The "naive scientific code" pattern: indexing with `a[[i, j]]` instead
/// of iterators. Every access goes through ndarray's bounds-checked
/// `Index`/`IndexMut`, and the byte offset `i*row_stride + j*col_stride` is
/// recomputed from the loop variables on every access rather than carried
/// as a running pointer -- exactly what GEPStrengthReduction and (given
/// `-rust-hpc-trust-bounds-checks`, since these indices are provably always
/// in range here) BoundsCheckElimination target.
#[inline(never)]
pub fn naive_indexed_add(a: &Array2<f32>, b: &Array2<f32>, out: &mut Array2<f32>) {
    let (rows, cols) = a.dim();
    for i in 0..rows {
        for j in 0..cols {
            out[[i, j]] = a[[i, j]] + b[[i, j]];
        }
    }
}

/// The output.s pattern: a small, fixed-size scratch buffer allocated fresh
/// every outer-loop iteration, used only locally within that iteration, and
/// freed before the next one starts -- never escapes, exactly what
/// HeapToStackPromotion targets. `vec![0.0f32; 64]` is `__rust_alloc` (zeroed
/// content via the Vec's own zero-fill, not necessarily `__rust_alloc_zeroed`
/// depending on how the optimizer lowers it) + `__rust_dealloc` on drop.
#[inline(never)]
pub fn scratch_buffer_kernel(data: &Array2<f32>) -> f32 {
    let (rows, cols) = data.dim();
    let mut sum = 0.0f32;
    for i in 0..rows {
        let mut scratch = vec![0.0f32; 64];
        let n = cols.min(64);
        for j in 0..n {
            scratch[j] = data[[i, j]] * 2.0;
        }
        for j in 0..n {
            sum += scratch[j];
        }
    }
    sum
}

/// The other output.s pattern: a hot numeric loop with debug/diagnostic
/// code sitting unconditionally right after it in the same function, the
/// way a forgotten debug print or an always-compiled telemetry call would.
/// `verbose` is always false in the benchmark call site below but the
/// compiler can't prove that, so the eprintln! machinery (formatting,
/// stdio locking) stays inlined into this function regardless -- exactly
/// what ColdPathOutlining targets.
#[inline(never)]
pub fn noisy_kernel(data: &Array2<f32>, verbose: bool) -> f32 {
    let (rows, cols) = data.dim();
    let mut sum = 0.0f32;
    for i in 0..rows {
        for j in 0..cols {
            sum += data[[i, j]] * data[[i, j]];
        }
    }
    if verbose {
        eprintln!("noisy_kernel: sum = {sum}");
    }
    sum
}

fn make_data(rows: usize, cols: usize) -> Array2<f32> {
    Array2::from_shape_fn((rows, cols), |(i, j)| ((i * 31 + j * 17) % 97) as f32 * 0.5)
}

fn bench<F: FnMut()>(name: &str, iters: u32, mut f: F) {
    // Warm up (page faults, first-touch cache effects) before timing.
    for _ in 0..iters.min(10) {
        f();
    }
    let start = Instant::now();
    for _ in 0..iters {
        f();
    }
    let elapsed = start.elapsed();
    let per_iter_ns = elapsed.as_nanos() as f64 / iters as f64;
    println!("{name:<24} {per_iter_ns:>12.1} ns/iter   ({iters} iters, {elapsed:?} total)");
}

fn main() {
    let rows = 256usize;
    let cols = 256usize;
    let a = make_data(rows, cols);
    let b = make_data(rows, cols);
    let mut out = Array2::<f32>::zeros((rows, cols));

    bench("zip_clean", 2000, || {
        zip_clean(black_box(&a), black_box(&b), black_box(&mut out));
        black_box(&out);
    });

    bench("naive_indexed_add", 2000, || {
        naive_indexed_add(black_box(&a), black_box(&b), black_box(&mut out));
        black_box(&out);
    });

    bench("scratch_buffer_kernel", 2000, || {
        let s = scratch_buffer_kernel(black_box(&a));
        black_box(s);
    });

    bench("noisy_kernel", 2000, || {
        let s = noisy_kernel(black_box(&a), black_box(false));
        black_box(s);
    });
}
