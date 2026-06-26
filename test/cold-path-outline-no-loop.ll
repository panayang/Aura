; A call to an allowlisted function outside any loop must not be outlined:
; this pass only targets hot-loop bodies, not every matching call in the
; whole function. Outlining a call that's already outside a loop would be
; pure overhead (an extra call indirection) for no benefit.

declare i32 @"core::fmt::write"(ptr, ptr) unnamed_addr

define i32 @not_in_a_loop(ptr %p) {
entry:
  %r = call i32 @"core::fmt::write"(ptr %p, ptr null)
  ret i32 %r
}

; CHECK: entry:
; CHECK-NEXT: %r = call i32 @"core::fmt::write"(ptr %p, ptr null)
; CHECK-NEXT: ret i32 %r
