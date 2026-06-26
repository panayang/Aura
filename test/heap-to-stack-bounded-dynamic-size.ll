; The size argument isn't a literal constant, but `and %n, 63` is provably
; bounded to [0, 63] -- ScalarEvolution can derive that range without us
; reimplementing value-range analysis. The alloc call is outside any loop,
; so a dynamically-sized alloca here is safe (no per-iteration stack
; growth). Should promote to a dynamic `alloca i8, i64 %size`.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define void @bounded_dynamic_size(i64 %n, ptr %sink) {
entry:
  %size = and i64 %n, 63
  %buf = call ptr @__rust_alloc(i64 %size, i64 8)
  call void @llvm.memcpy.p0.p0.i64(ptr %sink, ptr %buf, i64 %size, i1 false)
  call void @__rust_dealloc(ptr %buf, i64 %size, i64 8)
  ret void
}

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

; CHECK-NOT: call ptr @__rust_alloc
; CHECK-NOT: call void @__rust_dealloc
; CHECK: %size = and i64 %n, 63
; CHECK-NEXT: %rust.hpc.h2s = alloca i8, i64 %size, align 8
; CHECK-NEXT: call void @llvm.memcpy.p0.p0.i64(ptr %sink, ptr %rust.hpc.h2s, i64 %size, i1 false)
