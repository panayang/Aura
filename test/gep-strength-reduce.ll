; Driven by test/run_ll_test.cmake (no llvm-lit in this binary LLVM
; distribution), equivalent to:
;   opt -load-pass-plugin=RustHpcPasses -passes=rust-hpc-gep-strength-reduce -S % | FileCheck %
;
; Mirrors the ndarray::Zip pattern from output.s: a float* base pointer,
; loop-invariant stride, and an index recomputed as `i * stride` on every
; iteration via a `mul` inside the loop body instead of being carried as a
; running pointer. The pass should rewrite this into a loop-carried pointer
; increment and eliminate the per-iteration multiply.

define void @stride_mul_loop(ptr %base, i64 %stride, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %idx = mul i64 %i, %stride
  %gep = getelementptr inbounds float, ptr %base, i64 %idx
  store float 1.0, ptr %gep, align 4
  %i.next = add i64 %i, 1
  %cond = icmp ult i64 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-NOT: mul i64 %i, %stride
; CHECK: loop:
; CHECK: %[[PTR:.*]] = phi ptr [ %[[NEXT:.*]], %loop ], [ %base, %entry ]
; CHECK: store float 1.000000e+00, ptr %[[PTR]], align 4
; CHECK: %[[NEXT]] = getelementptr i8, ptr %[[PTR]], i64 {{.*}}
