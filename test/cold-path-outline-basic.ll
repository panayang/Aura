; A hot numeric loop with an incidental println-equivalent call sitting in
; its own block in the loop body (the realistic shape: header does the
; trip-count check, a separate body block does the per-iteration work). The
; body block isn't the loop's header or latch, so it should be outlined
; into a separate, Cold/noinline function, leaving only a call in its place.

declare i32 @"core::fmt::write"(ptr, ptr) unnamed_addr

define void @hot_loop_with_print(ptr %data, i64 %n) {
entry:
  br label %loop.header

loop.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop.latch ]
  %cond = icmp ult i64 %i, %n
  br i1 %cond, label %loop.body, label %exit

loop.body:
  %gep = getelementptr inbounds float, ptr %data, i64 %i
  %v = load float, ptr %gep, align 4
  %r = call i32 @"core::fmt::write"(ptr %gep, ptr null)
  br label %loop.latch

loop.latch:
  %i.next = add i64 %i, 1
  br label %loop.header

exit:
  ret void
}

; CHECK: loop.header:
; CHECK-NOT: call i32 @"core::fmt::write"
; CHECK: call void @{{.*}}(ptr %data, i64 %i) #[[ATTR:[0-9]+]]
; CHECK: loop.latch:
; CHECK: define internal void @{{.*}}(ptr %data, i64 %i) #[[ATTR]]
; CHECK: call i32 @"core::fmt::write"
; CHECK: attributes #[[ATTR]] = { cold noinline }
