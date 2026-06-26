; Size exceeds -rust-hpc-max-promotable-alloca-size (default 8192): even
; though it's constant and otherwise perfectly safe to promote, the cap
; exists specifically to avoid blowing the stack on a large allocation, so
; this must be left on the heap.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define void @too_large(i64 %n) {
entry:
  %buf = call ptr @__rust_alloc(i64 1048576, i64 8)
  call void @__rust_dealloc(ptr %buf, i64 1048576, i64 8)
  ret void
}

; CHECK: %buf = call ptr @__rust_alloc(i64 1048576, i64 8)
; CHECK-NEXT: call void @__rust_dealloc(ptr %buf, i64 1048576, i64 8)
