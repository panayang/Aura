; With -rust-hpc-cold-path-outline=0, the pass must be a complete no-op even
; on IR that would otherwise be outlined.

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

; CHECK: loop.body:
; CHECK-NEXT: %gep = getelementptr inbounds float, ptr %data, i64 %i
; CHECK-NEXT: %v = load float, ptr %gep, align 4
; CHECK-NEXT: %r = call i32 @"core::fmt::write"(ptr %gep, ptr null)
