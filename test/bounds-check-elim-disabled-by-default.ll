; Without -rust-hpc-trust-bounds-checks, the pass must be a complete no-op,
; even on IR that would otherwise match every structural check. This is the
; safety-by-default behavior: the flag is the only thing that should ever
; cause a real bounds check to disappear.

declare void @core..panicking..panic_bounds_check(i64, i64, ptr) unnamed_addr noreturn

define float @index_with_check(ptr %data, i64 %idx, i64 %len) {
entry:
  %cmp = icmp uge i64 %idx, %len
  br i1 %cmp, label %panic, label %safe

panic:
  call void @core..panicking..panic_bounds_check(i64 %idx, i64 %len, ptr null)
  unreachable

safe:
  %gep = getelementptr inbounds float, ptr %data, i64 %idx
  %v = load float, ptr %gep, align 4
  ret float %v
}

; CHECK: br i1 %cmp, label %panic, label %safe
; CHECK: call void @core..panicking..panic_bounds_check
