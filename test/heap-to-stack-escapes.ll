; The buffer is passed to an unknown external function before being freed.
; CaptureTracking conservatively assumes an arbitrary external call may
; stash the pointer somewhere outside the function, so this must NOT be
; promoted even though there's exactly one, post-dominating dealloc call.

declare ptr @__rust_alloc(i64, i64)
declare void @__rust_dealloc(ptr, i64, i64)
declare void @maybe_stashes_pointer_somewhere(ptr)

define void @passes_buffer_to_unknown_call(i64 %n) {
entry:
  %buf = call ptr @__rust_alloc(i64 64, i64 8)
  call void @maybe_stashes_pointer_somewhere(ptr %buf)
  call void @__rust_dealloc(ptr %buf, i64 64, i64 8)
  ret void
}

; CHECK: %buf = call ptr @__rust_alloc(i64 64, i64 8)
; CHECK: call void @maybe_stashes_pointer_somewhere(ptr %buf)
; CHECK: call void @__rust_dealloc(ptr %buf, i64 64, i64 8)
