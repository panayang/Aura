; Size is a runtime value, not a compile-time constant -- v1 only promotes
; fixed-size allocations (turning this into a `alloca i8, i64 %n` dynamic
; alloca would need a separate, much more careful safety argument about
; stack-overflow risk that this pass doesn't make). Must be left alone.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define void @dynamic_size(i64 %n) {
entry:
  %buf = call ptr @__rust_alloc(i64 %n, i64 8)
  call void @__rust_dealloc(ptr %buf, i64 %n, i64 8)
  ret void
}

; CHECK: %buf = call ptr @__rust_alloc(i64 %n, i64 8)
; CHECK-NEXT: call void @__rust_dealloc(ptr %buf, i64 %n, i64 8)
