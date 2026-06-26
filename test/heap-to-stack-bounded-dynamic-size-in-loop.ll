; Same provably-bounded size as heap-to-stack-bounded-dynamic-size.ll, but
; the alloc call is inside a loop. A non-constant-sized `alloca` outside the
; entry block grows the current stack frame every time it executes and
; isn't reclaimed until the function returns -- so promoting this without
; further care would leak stack space every iteration, the same hazard as a
; C VLA inside a loop body. The pass handles this the same way Clang does
; for that case: bracket each logical instance with
; llvm.stacksave/llvm.stackrestore so the frame returns to its prior size
; before the next iteration's allocation.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define void @bounded_dynamic_size_in_loop(i64 %n, i64 %count, ptr %sink) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %size = and i64 %n, 63
  %buf = call ptr @__rust_alloc(i64 %size, i64 8)
  call void @llvm.memcpy.p0.p0.i64(ptr %sink, ptr %buf, i64 %size, i1 false)
  call void @__rust_dealloc(ptr %buf, i64 %size, i64 8)
  %i.next = add i64 %i, 1
  %cond = icmp ult i64 %i.next, %count
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

; CHECK-NOT: call ptr @__rust_alloc
; CHECK-NOT: call void @__rust_dealloc
; CHECK: loop:
; CHECK: %[[SAVE:.*]] = call ptr @llvm.stacksave.p0()
; CHECK-NEXT: %rust.hpc.h2s = alloca i8, i64 %size, align 8
; CHECK: call void @llvm.memcpy.p0.p0.i64(ptr %sink, ptr %rust.hpc.h2s, i64 %size, i1 false)
; CHECK-NEXT: call void @llvm.stackrestore.p0(ptr %[[SAVE]])
