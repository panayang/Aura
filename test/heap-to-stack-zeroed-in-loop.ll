; __rust_alloc_zeroed inside a loop: each "logical" allocation must still be
; zero-filled, since reusing one stack slot across iterations would
; otherwise leak the previous iteration's contents where a fresh heap
; allocation wouldn't have. The promoted code must re-memset at the
; original call site on every iteration.

declare ptr @__rust_alloc_zeroed(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define void @zero_each_iteration(i64 %n, ptr %sink) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %buf = call ptr @__rust_alloc_zeroed(i64 64, i64 8)
  call void @llvm.memcpy.p0.p0.i64(ptr %sink, ptr %buf, i64 64, i1 false)
  call void @__rust_dealloc(ptr %buf, i64 64, i64 8)
  %i.next = add i64 %i, 1
  %cond = icmp ult i64 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

; CHECK-NOT: call ptr @__rust_alloc_zeroed
; CHECK-NOT: call void @__rust_dealloc
; CHECK: entry:
; CHECK-NEXT: %rust.hpc.h2s = alloca [64 x i8], align 8
; CHECK: loop:
; CHECK: call void @llvm.memset.p0.i64(ptr align 8 %rust.hpc.h2s, i8 0, i64 64, i1 false)
; CHECK-NEXT: call void @llvm.memcpy
