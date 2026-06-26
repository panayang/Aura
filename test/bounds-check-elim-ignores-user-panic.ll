; A generic user-code panic (e.g. an `.expect()` failure or assertion),
; structurally identical to a bounds check (single-predecessor guard block,
; noreturn call, unreachable terminator) but calling a different panic
; entry point and not sharing operands with the comparison in the same way.
; This must never be removed, even with -rust-hpc-trust-bounds-checks: the
; flag only authorizes removing *bounds checks*, not panics in general.

declare void @core..panicking..panic(ptr) unnamed_addr noreturn

define i64 @check_positive(i64 %x) {
entry:
  %cmp = icmp slt i64 %x, 0
  br i1 %cmp, label %panic, label %safe

panic:
  call void @core..panicking..panic(ptr null)
  unreachable

safe:
  ret i64 %x
}

; CHECK: br i1 %cmp, label %panic, label %safe
; CHECK: call void @core..panicking..panic(ptr null)
