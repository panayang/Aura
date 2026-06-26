; Regression test for a precision bug caught during review: a block of
; ordinary, unconditionally-executed hot setup work (`hot.preamble`) sits
; directly between the loop header and the real fork (`fork`). Both
; `hot.preamble` and the fork's "skip this" successor (`fast.continue`) sit
; on the same dominator-tree idom chain as the seed call, so a region-growth
; algorithm based on dominance alone would sweep `hot.preamble` into the
; outlined cold function even though it runs on *every* iteration
; regardless of which way `fork` branches. The fix checks that every
; successor of a candidate ancestor can still reach the seed before
; absorbing it; `fast.continue` can't (it bypasses the slow path entirely),
; so climbing must stop at `fork`'s direct child and `hot.preamble` must
; stay in the hot loop.

declare i32 @"core::fmt::write"(ptr, ptr) unnamed_addr

define void @hot_loop_with_preamble(ptr %data, i64 %n, i64 %flag) {
entry:
  br label %loop.header

loop.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop.latch ]
  %cond = icmp ult i64 %i, %n
  br i1 %cond, label %hot.preamble, label %exit

hot.preamble:
  %scaled = mul i64 %i, 7
  br label %fork

fork:
  %rare = icmp eq i64 %flag, 0
  br i1 %rare, label %slow.path, label %fast.continue

slow.path:
  %gep = getelementptr inbounds float, ptr %data, i64 %scaled
  %r = call i32 @"core::fmt::write"(ptr %gep, ptr null)
  br label %loop.latch

fast.continue:
  br label %loop.latch

loop.latch:
  %i.next = add i64 %i, 1
  br label %loop.header

exit:
  ret void
}

; CHECK: hot.preamble:
; CHECK-NEXT: %scaled = mul i64 %i, 7
; CHECK-NEXT: br label %fork
; CHECK: fork:
; CHECK: br i1 %rare, label %{{.*}}, label %fast.continue
; CHECK-NOT: call i32 @"core::fmt::write"
; CHECK: call void @{{.*}}(
