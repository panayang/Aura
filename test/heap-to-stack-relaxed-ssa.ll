; Mimics what a Box<T>'s single-pointer-field struct looks like when
; SROA/mem2reg hasn't eliminated it: the alloc's return value is stored
; into a local `ptr`-typed alloca (`%box.slot`) immediately, and the
; dealloc call's pointer argument is a reload of that slot rather than the
; alloc call's SSA value directly. v1 of this pass required direct SSA
; equality and would have left this on the heap; v2 verifies (not assumes)
; that %box.slot has exactly one store anywhere in the function, that the
; store dominates the reload, and that the stored value is the alloc call
; -- a sound justification for treating the reload as the same pointer.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define float @reload_from_box_slot(i64 %n) {
entry:
  %box.slot = alloca ptr
  %buf = call ptr @__rust_alloc(i64 64, i64 8)
  store ptr %buf, ptr %box.slot
  %reloaded.for.use = load ptr, ptr %box.slot
  store float 1.0, ptr %reloaded.for.use, align 4
  %reloaded.for.free = load ptr, ptr %box.slot
  %v = load float, ptr %reloaded.for.free, align 4
  call void @__rust_dealloc(ptr %reloaded.for.free, i64 64, i64 8)
  ret float %v
}

; CHECK-NOT: call ptr @__rust_alloc
; CHECK-NOT: call void @__rust_dealloc
; CHECK: %rust.hpc.h2s = alloca [64 x i8], align 8
