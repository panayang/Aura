; Same guard, opposite branch encoding: `icmp ult %idx, %len` true -> safe,
; false -> panic. The pass shouldn't care which way the predicate points;
; it only needs the panic successor to be the structurally-verified guard
; target.

declare void @core..panicking..panic_bounds_check(i64, i64, ptr) unnamed_addr noreturn

define float @index_with_check(ptr %data, i64 %idx, i64 %len) {
entry:
  %cmp = icmp ult i64 %idx, %len
  br i1 %cmp, label %safe, label %panic

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
