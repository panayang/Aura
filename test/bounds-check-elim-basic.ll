; Typical rustc-emitted bounds check: `icmp uge %idx, %len` true -> panic.
; The pass is a no-op unless explicitly enabled, so this test runs the
; pipeline twice via two RUN-equivalents is not supported by our harness
; (single command per test), so this test only exercises the *enabled*
; path; bounds-check-elim-disabled-by-default.ll covers the off-by-default
; behavior.

declare void @core..panicking..panic_bounds_check(i64, i64, ptr) unnamed_addr noreturn

; Mirrors the realistic shape rustc emits: the panic declaration is always
; `noreturn`, which the pass requires as part of its structural check that
; the candidate block is purely a guard target.

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

; CHECK-NOT: icmp
; CHECK-NOT: br i1
; CHECK: %gep = getelementptr inbounds float, ptr %data, i64 %idx
; CHECK-NEXT: %v = load float, ptr %gep, align 4
; CHECK-NEXT: ret float %v
