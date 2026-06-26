; Nested loops: outer loop over rows, inner loop over columns, address
; computed via explicit multiplies on each level (`i * rowStride + j *
; colStride`), exactly the shape that shows up when ndarray-style code
; manually flattens a 2D index instead of using nested GEP indexing. SCEV
; represents this as a nested AddRecExpr ({{base,+,rowStride}<outer>,+,
; colStride}<inner>) and the pass should strength-reduce both levels: the
; inner loop gets a per-iteration pointer add, and the outer loop's
; recurrence (the inner loop's start pointer advancing by rowStride each
; outer iteration) should also collapse to a pointer add instead of
; recomputing `i * rowStride` from scratch.

define void @row_col_walk(ptr %base, i64 %rowStride, i64 %colStride, i64 %rows, i64 %cols) {
entry:
  br label %outer

outer:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  %rowOff = mul i64 %i, %rowStride
  br label %inner

inner:
  %j = phi i64 [ 0, %outer ], [ %j.next, %inner ]
  %colOff = mul i64 %j, %colStride
  %off = add i64 %rowOff, %colOff
  %gep = getelementptr inbounds float, ptr %base, i64 %off
  store float 3.0, ptr %gep, align 4
  %j.next = add i64 %j, 1
  %innerCond = icmp ult i64 %j.next, %cols
  br i1 %innerCond, label %inner, label %outer.latch

outer.latch:
  %i.next = add i64 %i, 1
  %outerCond = icmp ult i64 %i.next, %rows
  br i1 %outerCond, label %outer, label %exit

exit:
  ret void
}

; CHECK-NOT: mul i64 %j, %colStride
; CHECK: inner:
; CHECK: %[[PTR:.*]] = phi ptr [ %[[NEXT:.*]], %inner ], [ {{.*}}, %outer ]
; CHECK: store float 3.000000e+00, ptr %[[PTR]], align 4
; CHECK: %[[NEXT]] = getelementptr i8, ptr %[[PTR]], i64 {{.*}}
