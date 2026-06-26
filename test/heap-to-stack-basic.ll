; Mirrors the output.s scenario directly: a fixed-size buffer allocated via
; __rust_alloc, written to in a loop, read back, and freed once at the end
; with no other uses -- never escapes, single dealloc site, constant size.
; Should become a single stack alloca with both calls removed entirely.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define float @sum_scratch_buffer(i64 %n) {
entry:
  %buf = call ptr @__rust_alloc(i64 1024, i64 16)
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %gep = getelementptr inbounds float, ptr %buf, i64 %i
  store float 1.0, ptr %gep, align 4
  %i.next = add i64 %i, 1
  %cond = icmp ult i64 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  %last = getelementptr inbounds float, ptr %buf, i64 0
  %v = load float, ptr %last, align 4
  call void @__rust_dealloc(ptr %buf, i64 1024, i64 16)
  ret float %v
}

; CHECK-NOT: call ptr @__rust_alloc
; CHECK-NOT: call void @__rust_dealloc
; CHECK: %rust.hpc.h2s = alloca [1024 x i8], align 16
