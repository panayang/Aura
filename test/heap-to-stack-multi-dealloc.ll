; Two dealloc sites for the same pointer (cleanup duplicated across an
; early-return error path and the normal exit) -- v2 of this pass supports
; this directly via a forward graph search proving every path reaches
; exactly one of the dealloc sites, rather than requiring a single
; post-dominating site. Both call sites should disappear and the buffer
; should become one stack alloca shared by both paths.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)

define float @two_dealloc_sites(i64 %flag) {
entry:
  %buf = call ptr @__rust_alloc(i64 64, i64 8)
  store float 1.0, ptr %buf, align 4
  %cond = icmp eq i64 %flag, 0
  br i1 %cond, label %early_exit, label %normal

early_exit:
  call void @__rust_dealloc(ptr %buf, i64 64, i64 8)
  ret float 0.0

normal:
  %v = load float, ptr %buf, align 4
  call void @__rust_dealloc(ptr %buf, i64 64, i64 8)
  ret float %v
}

; CHECK-NOT: call ptr @__rust_alloc
; CHECK-NOT: call void @__rust_dealloc
; CHECK: entry:
; CHECK-NEXT: %rust.hpc.h2s = alloca [64 x i8], align 8
; CHECK: store float 1.000000e+00, ptr %rust.hpc.h2s, align 4
; CHECK: early_exit:
; CHECK-NEXT: ret float 0.000000e+00
; CHECK: normal:
; CHECK-NEXT: %v = load float, ptr %rust.hpc.h2s, align 4
; CHECK-NEXT: ret float %v
