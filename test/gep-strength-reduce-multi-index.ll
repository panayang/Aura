; Multi-index GEP: a fixed-width 2D array `[1024 x [64 x float]]`, walked by
; a single loop varying the row index while the column index is a function
; argument (loop-invariant). v1 of this pass only matched single-index GEPs;
; SCEV linearizes the whole multi-dimensional access into one flat affine
; expression, so this should strength-reduce just like the single-index
; case, folding the row-stride multiply into a single per-iteration pointer
; add.

define void @row_major_walk(ptr %arr, i64 %col, i64 %n) {
entry:
  br label %loop

loop:
  %row = phi i64 [ 0, %entry ], [ %row.next, %loop ]
  %gep = getelementptr inbounds [1024 x [64 x float]], ptr %arr, i64 0, i64 %row, i64 %col
  store float 2.0, ptr %gep, align 4
  %row.next = add i64 %row, 1
  %cond = icmp ult i64 %row.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-NOT: getelementptr inbounds [1024 x [64 x float]]
; CHECK: loop:
; CHECK: %[[PTR:.*]] = phi ptr [ %[[NEXT:.*]], %loop ], [ {{.*}}, %entry ]
; CHECK: store float 2.000000e+00, ptr %[[PTR]], align 4
; CHECK: %[[NEXT]] = getelementptr i8, ptr %[[PTR]], i64 {{.*}}
