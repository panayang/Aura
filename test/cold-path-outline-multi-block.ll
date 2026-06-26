; Mirrors the real shape from output.s: a hot loop whose body, on a rare
; path, falls into a multi-block slow path -- a TLS-slot check, a CAS retry
; loop allocating a thread id (no recognizable call at all, just inlined
; atomics), and only then a call to the allowlisted fmt::write. v1 of this
; pass could only have found the fmt::write block itself; this checks that
; the whole slow path (tls.check, cas.retry, cas.body, the lot) gets pulled
; out together via structural (dominance-based) region growth, leaving the
; hot loop with nothing but a single cold call.

declare i32 @"core::fmt::write"(ptr, ptr) unnamed_addr

define void @hot_loop_with_slow_path(ptr %data, i64 %n, ptr %tls_slot, ptr %counter) {
entry:
  br label %loop.header

loop.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop.latch ]
  %cond = icmp ult i64 %i, %n
  br i1 %cond, label %tls.check, label %exit

tls.check:
  %tls.val = load i64, ptr %tls_slot, align 8
  %tls.miss = icmp eq i64 %tls.val, 0
  br i1 %tls.miss, label %cas.retry, label %do.write

cas.retry:
  %cur = phi i64 [ %tls.val, %tls.check ], [ %next, %cas.retry ]
  %next = add i64 %cur, 1
  %cas = cmpxchg ptr %counter, i64 %cur, i64 %next seq_cst seq_cst
  %cas.val = extractvalue { i64, i1 } %cas, 0
  %cas.ok = extractvalue { i64, i1 } %cas, 1
  br i1 %cas.ok, label %cas.done, label %cas.retry

cas.done:
  store i64 %next, ptr %tls_slot, align 8
  br label %do.write

do.write:
  %gep = getelementptr inbounds float, ptr %data, i64 %i
  %r = call i32 @"core::fmt::write"(ptr %gep, ptr null)
  br label %loop.latch

loop.latch:
  %i.next = add i64 %i, 1
  br label %loop.header

exit:
  ret void
}

; CHECK: loop.header:
; CHECK-NOT: tls.check:
; CHECK-NOT: cas.retry:
; CHECK-NOT: call i32 @"core::fmt::write"
; CHECK: call void @{{.*}}(ptr {{.*}}) #[[ATTR:[0-9]+]]
; CHECK: loop.latch:
; CHECK: define internal void @{{.*}}#[[ATTR]] {
; CHECK: tls.check:
; CHECK: cas.retry:
; CHECK: call i32 @"core::fmt::write"
; CHECK: attributes #[[ATTR]] = { cold noinline }
