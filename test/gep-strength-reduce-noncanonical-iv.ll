; Non-canonical induction variable: counts *down* from %n to 0, stepping by
; 5 each iteration (not the "starts at 0, steps by 1" shape v1 of this pass
; required). ScalarEvolution describes this as the affine recurrence
; {%n,+,-5}<loop> regardless of the direction or step size, so the rewrite
; should apply exactly as it would for a counting-up-by-1 loop.

define void @countdown_stride_loop(ptr %base, i64 %stride, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ %n, %entry ], [ %i.next, %loop ]
  %idx = mul i64 %i, %stride
  %gep = getelementptr inbounds float, ptr %base, i64 %idx
  store float 4.0, ptr %gep, align 4
  %i.next = sub i64 %i, 5
  %cond = icmp ugt i64 %i.next, 0
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-NOT: mul i64 %i, %stride
; CHECK: loop:
; CHECK: %[[PTR:.*]] = phi ptr [ %[[NEXT:.*]], %loop ], [ {{.*}}, %entry ]
; CHECK: store float 4.000000e+00, ptr %[[PTR]], align 4
; CHECK: %[[NEXT]] = getelementptr i8, ptr %[[PTR]], i64 {{.*}}
