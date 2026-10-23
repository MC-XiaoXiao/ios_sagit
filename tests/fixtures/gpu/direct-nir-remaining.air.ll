; SPDX-License-Identifier: BSD-2-Clause
; Direct frontend regression inputs, independently checked by the CPU oracle.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"

@shared64 = internal addrspace(3) global [4 x i64] undef, align 8

define void @multi_exit(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
entry:
  br label %loop

loop:                                             ; preds = %body, %entry
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %x = phi i32 [ 11, %entry ], [ %y, %body ]
  %y = phi i32 [ 23, %entry ], [ %x, %body ]
  %limit = and i32 %id, 7
  %more = icmp ule i32 %i, %limit
  br i1 %more, label %body, label %done

body:                                             ; preds = %loop
  %next = add i32 %i, 1
  %zero = icmp eq i32 %id, 0
  br i1 %zero, label %early, label %loop

early:                                            ; preds = %body
  store i32 99, ptr addrspace(1) %out, align 4
  ret void

done:                                             ; preds = %loop
  %p = getelementptr i32, ptr addrspace(1) %out, i32 %id
  %sum = add i32 %x, %i
  store i32 %sum, ptr addrspace(1) %p, align 4
  ret void
}

define void @irreducible(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
entry:
  %limit0 = and i32 %id, 7
  %limit = add i32 %limit0, 2
  %odd0 = and i32 %id, 1
  %odd = icmp ne i32 %odd0, 0
  br i1 %odd, label %left, label %right

left:                                             ; preds = %right, %entry
  %li = phi i32 [ 0, %entry ], [ %ri1, %right ]
  %lx = phi i32 [ 1, %entry ], [ %ry, %right ]
  %ly = phi i32 [ 7, %entry ], [ %rx1, %right ]
  %li1 = add i32 %li, 1
  %lx1 = add i32 %lx, %id
  %lm = icmp ult i32 %li1, %limit
  br i1 %lm, label %right, label %exit_l

right:                                            ; preds = %left, %entry
  %ri = phi i32 [ 0, %entry ], [ %li1, %left ]
  %rx = phi i32 [ 1, %entry ], [ %ly, %left ]
  %ry = phi i32 [ 7, %entry ], [ %lx1, %left ]
  %ri1 = add i32 %ri, 1
  %rx1 = add i32 %rx, 3
  %rm = icmp ult i32 %ri1, %limit
  br i1 %rm, label %left, label %exit_r

exit_l:                                           ; preds = %left
  %lp = getelementptr i32, ptr addrspace(1) %out, i32 %id
  store i32 %lx1, ptr addrspace(1) %lp, align 4
  ret void

exit_r:                                           ; preds = %right
  %rp = getelementptr i32, ptr addrspace(1) %out, i32 %id
  store i32 %rx1, ptr addrspace(1) %rp, align 4
  ret void
}

define void @pointer_phi(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
entry:
  %base = mul i32 %id, 8
  %pa = getelementptr i32, ptr addrspace(1) %a, i32 %base
  %pb = getelementptr i32, ptr addrspace(1) %b, i32 %base
  %odd0 = and i32 %id, 1
  %odd = icmp ne i32 %odd0, 0
  %start = select i1 %odd, ptr addrspace(1) %pa, ptr addrspace(1) %pb
  %other = select i1 %odd, ptr addrspace(1) %pb, ptr addrspace(1) %pa
  br label %loop

loop:                                             ; preds = %body, %entry
  %p = phi ptr addrspace(1) [ %start, %entry ], [ %qn, %body ]
  %q = phi ptr addrspace(1) [ %other, %entry ], [ %pn, %body ]
  %i = phi i32 [ 0, %entry ], [ %in, %body ]
  %sum = phi i32 [ 0, %entry ], [ %sn, %body ]
  %more = icmp ult i32 %i, 4
  br i1 %more, label %body, label %done

body:                                             ; preds = %loop
  %v = load i32, ptr addrspace(1) %p, align 4
  %sn = add i32 %sum, %v
  %in = add i32 %i, 1
  %pn = getelementptr i32, ptr addrspace(1) %p, i32 1
  %qn = getelementptr i32, ptr addrspace(1) %q, i32 1
  br label %loop

done:                                             ; preds = %loop
  %old = atomicrmw add ptr addrspace(1) %p, i32 %id monotonic, align 4
  %result = add i32 %sum, %old
  %dest = getelementptr i32, ptr addrspace(1) %out, i32 %id
  store i32 %result, ptr addrspace(1) %dest, align 4
  ret void
}

define void @wide_div_3(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %v3 = udiv i64 %x, %y
  %d3 = getelementptr i64, ptr addrspace(1) %dest, i32 3
  store i64 %v3, ptr addrspace(1) %d3, align 4
  ret void
}

define void @wide_div_4(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %v4 = urem i64 %x, %y
  %d4 = getelementptr i64, ptr addrspace(1) %dest, i32 4
  store i64 %v4, ptr addrspace(1) %d4, align 4
  ret void
}

define void @wide_integer_0(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %v0 = add i64 %x, %y
  %d0 = getelementptr i64, ptr addrspace(1) %dest, i32 0
  store i64 %v0, ptr addrspace(1) %d0, align 4
  %v1 = sub i64 %x, %y
  %d1 = getelementptr i64, ptr addrspace(1) %dest, i32 1
  store i64 %v1, ptr addrspace(1) %d1, align 4
  %v2 = mul i64 %x, %y
  %d2 = getelementptr i64, ptr addrspace(1) %dest, i32 2
  store i64 %v2, ptr addrspace(1) %d2, align 4
  ret void
}

define void @wide_div_5(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %v5 = sdiv i64 %x, %y
  %d5 = getelementptr i64, ptr addrspace(1) %dest, i32 5
  store i64 %v5, ptr addrspace(1) %d5, align 4
  ret void
}

define void @wide_div_6(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %v6 = srem i64 %x, %y
  %d6 = getelementptr i64, ptr addrspace(1) %dest, i32 6
  store i64 %v6, ptr addrspace(1) %d6, align 4
  ret void
}

define void @wide_integer_1(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %v7 = and i64 %x, %y
  %d7 = getelementptr i64, ptr addrspace(1) %dest, i32 7
  store i64 %v7, ptr addrspace(1) %d7, align 4
  %v8 = or i64 %x, %y
  %d8 = getelementptr i64, ptr addrspace(1) %dest, i32 8
  store i64 %v8, ptr addrspace(1) %d8, align 4
  %v9 = xor i64 %x, %y
  %d9 = getelementptr i64, ptr addrspace(1) %dest, i32 9
  store i64 %v9, ptr addrspace(1) %d9, align 4
  ret void
}

define void @wide_integer_2(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %shift0 = zext i32 %id to i64
  %shift = and i64 %shift0, 63
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %v10 = shl i64 %x, %shift
  %d10 = getelementptr i64, ptr addrspace(1) %dest, i32 10
  store i64 %v10, ptr addrspace(1) %d10, align 4
  %v11 = lshr i64 %x, %shift
  %d11 = getelementptr i64, ptr addrspace(1) %dest, i32 11
  store i64 %v11, ptr addrspace(1) %d11, align 4
  %v12 = ashr i64 %x, %shift
  %d12 = getelementptr i64, ptr addrspace(1) %dest, i32 12
  store i64 %v12, ptr addrspace(1) %d12, align 4
  %c13 = icmp eq i64 %x, %y
  %v13 = zext i1 %c13 to i64
  %d13 = getelementptr i64, ptr addrspace(1) %dest, i32 13
  store i64 %v13, ptr addrspace(1) %d13, align 4
  %c14 = icmp ne i64 %x, %y
  %v14 = zext i1 %c14 to i64
  %d14 = getelementptr i64, ptr addrspace(1) %dest, i32 14
  store i64 %v14, ptr addrspace(1) %d14, align 4
  %c15 = icmp ult i64 %x, %y
  %v15 = zext i1 %c15 to i64
  %d15 = getelementptr i64, ptr addrspace(1) %dest, i32 15
  store i64 %v15, ptr addrspace(1) %d15, align 4
  %c16 = icmp ule i64 %x, %y
  %v16 = zext i1 %c16 to i64
  %d16 = getelementptr i64, ptr addrspace(1) %dest, i32 16
  store i64 %v16, ptr addrspace(1) %d16, align 4
  %c17 = icmp ugt i64 %x, %y
  %v17 = zext i1 %c17 to i64
  %d17 = getelementptr i64, ptr addrspace(1) %dest, i32 17
  store i64 %v17, ptr addrspace(1) %d17, align 4
  %c18 = icmp uge i64 %x, %y
  %v18 = zext i1 %c18 to i64
  %d18 = getelementptr i64, ptr addrspace(1) %dest, i32 18
  store i64 %v18, ptr addrspace(1) %d18, align 4
  %c19 = icmp slt i64 %x, %y
  %v19 = zext i1 %c19 to i64
  %d19 = getelementptr i64, ptr addrspace(1) %dest, i32 19
  store i64 %v19, ptr addrspace(1) %d19, align 4
  %c20 = icmp sle i64 %x, %y
  %v20 = zext i1 %c20 to i64
  %d20 = getelementptr i64, ptr addrspace(1) %dest, i32 20
  store i64 %v20, ptr addrspace(1) %d20, align 4
  %c21 = icmp sgt i64 %x, %y
  %v21 = zext i1 %c21 to i64
  %d21 = getelementptr i64, ptr addrspace(1) %dest, i32 21
  store i64 %v21, ptr addrspace(1) %d21, align 4
  %c22 = icmp sge i64 %x, %y
  %v22 = zext i1 %c22 to i64
  %d22 = getelementptr i64, ptr addrspace(1) %dest, i32 22
  store i64 %v22, ptr addrspace(1) %d22, align 4
  ret void
}

define void @wide_integer_3(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %pb = getelementptr i64, ptr addrspace(1) %b, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %raw_y = load i64, ptr addrspace(1) %pb, align 4
  %y = or i64 %raw_y, 1
  %slot = mul i32 %id, 32
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %slot
  %c15 = icmp ult i64 %x, %y
  %fs = sitofp i64 %x to float
  %fu = uitofp i64 %x to float
  %fb0 = bitcast float %fs to i32
  %fb1 = bitcast float %fu to i32
  %fv0 = zext i32 %fb0 to i64
  %fv1 = zext i32 %fb1 to i64
  %back = bitcast i64 %x to <4 x i16>
  %rot = shufflevector <4 x i16> %back, <4 x i16> poison, <4 x i32> <i32 3, i32 2, i32 1, i32 0>
  %rot64 = bitcast <4 x i16> %rot to i64
  %chosen = select i1 %c15, i64 %x, i64 %y
  %parity = trunc i32 %id to i1
  %signed = sext i1 %parity to i64
  %truncated = trunc i64 %x to i16
  %sext = sext i16 %truncated to i64
  %zext = zext i16 %truncated to i64
  %f0 = uitofp i32 %id to float
  %f1 = fmul float %f0, -1.500000e+00
  %i0 = fptosi float %f1 to i64
  %i1 = fptoui float %f0 to i64
  %d23 = getelementptr i64, ptr addrspace(1) %dest, i32 23
  store i64 %fv0, ptr addrspace(1) %d23, align 4
  %d24 = getelementptr i64, ptr addrspace(1) %dest, i32 24
  store i64 %fv1, ptr addrspace(1) %d24, align 4
  %d25 = getelementptr i64, ptr addrspace(1) %dest, i32 25
  store i64 %rot64, ptr addrspace(1) %d25, align 4
  %d26 = getelementptr i64, ptr addrspace(1) %dest, i32 26
  store i64 %chosen, ptr addrspace(1) %d26, align 4
  %d27 = getelementptr i64, ptr addrspace(1) %dest, i32 27
  store i64 %signed, ptr addrspace(1) %d27, align 4
  %d28 = getelementptr i64, ptr addrspace(1) %dest, i32 28
  store i64 %sext, ptr addrspace(1) %d28, align 4
  %d29 = getelementptr i64, ptr addrspace(1) %dest, i32 29
  store i64 %zext, ptr addrspace(1) %d29, align 4
  %d30 = getelementptr i64, ptr addrspace(1) %dest, i32 30
  store i64 %i0, ptr addrspace(1) %d30, align 4
  %d31 = getelementptr i64, ptr addrspace(1) %dest, i32 31
  store i64 %i1, ptr addrspace(1) %d31, align 4
  ret void
}

define void @wide_phi(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
entry:
  %local = alloca [8 x i64], align 8
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %x0 = load i64, ptr addrspace(1) %pa, align 4
  %y0 = xor i64 %x0, 4294967297
  %slot = and i32 %id, 7
  %lp = getelementptr [8 x i64], ptr %local, i32 0, i32 %slot
  store i64 %y0, ptr %lp, align 8
  %yinit = load i64, ptr %lp, align 8
  br label %loop

loop:                                             ; preds = %body, %entry
  %n = phi i64 [ 0, %entry ], [ %next, %body ]
  %x = phi i64 [ %x0, %entry ], [ %y, %body ]
  %y = phi i64 [ %yinit, %entry ], [ %new, %body ]
  %limit0 = and i32 %id, 7
  %limit = zext i32 %limit0 to i64
  %more = icmp ule i64 %n, %limit
  br i1 %more, label %body, label %done

body:                                             ; preds = %loop
  %next = add i64 %n, 1
  %new = add i64 %x, %y
  br label %loop

done:                                             ; preds = %loop
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %id
  store i64 %x, ptr addrspace(1) %dest, align 4
  ret void
}

define void @pointer_local_null(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
entry:
  %left = alloca i32, align 4
  %right = alloca i32, align 4
  store i32 13, ptr %left, align 4
  store i32 29, ptr %right, align 4
  %odd0 = and i32 %id, 1
  %odd = icmp ne i32 %odd0, 0
  %first = select i1 %odd, ptr %left, ptr %right
  %none0 = and i32 %id, 2
  %none = icmp ne i32 %none0, 0
  %selected = select i1 %none, ptr null, ptr %first
  %isnull = icmp eq ptr %selected, null
  br i1 %isnull, label %zero, label %read

read:                                             ; preds = %entry
  %loaded = load i32, ptr %selected, align 4
  br label %done

zero:                                             ; preds = %entry
  br label %done

done:                                             ; preds = %zero, %read
  %value = phi i32 [ 0, %zero ], [ %loaded, %read ]
  %dest = getelementptr i32, ptr addrspace(1) %out, i32 %id
  store i32 %value, ptr addrspace(1) %dest, align 4
  ret void
}

define void @wide_address(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %offset = load i64, ptr addrspace(1) %pa, align 4
  %source = getelementptr i8, ptr addrspace(1) %b, i64 %offset
  %value = load i64, ptr addrspace(1) %source, align 4
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %id
  store i64 %value, ptr addrspace(1) %dest, align 4
  ret void
}

define void @wide_shared(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
  %slot = and i32 %id, 3
  %pa = getelementptr i64, ptr addrspace(1) %a, i32 %id
  %x = load i64, ptr addrspace(1) %pa, align 4
  %sp = getelementptr [4 x i64], ptr addrspace(3) @shared64, i32 0, i32 %slot
  store i64 %x, ptr addrspace(3) %sp, align 8
  %loaded = load i64, ptr addrspace(3) %sp, align 8
  %value = add i64 %loaded, 4294967297
  %dest = getelementptr i64, ptr addrspace(1) %out, i32 %id
  store i64 %value, ptr addrspace(1) %dest, align 4
  ret void
}

declare <2 x float> @air.convert.f.v2f32.u.v2i64(<2 x i64>)
define void @wide_vector(ptr addrspace(1) %out, ptr addrspace(1) %a, ptr addrspace(1) %b, i32 %id) {
 %pa = getelementptr <2 x i64>, ptr addrspace(1) %a, i32 %id
 %x = load <2 x i64>, ptr addrspace(1) %pa, align 4
 %sum = add <2 x i64> %x, <i64 4294967297, i64 -4294967297>
 %f = call <2 x float> @air.convert.f.v2f32.u.v2i64(<2 x i64> %x)
 %base = mul i32 %id, 32
 %dest = getelementptr i8, ptr addrspace(1) %out, i32 %base
 store <2 x i64> %sum, ptr addrspace(1) %dest, align 4
 %fp = getelementptr i8, ptr addrspace(1) %dest, i32 16
 store <2 x float> %f, ptr addrspace(1) %fp, align 4
 ret void
}
!air.version = !{!0}
!air.kernel = !{!1, !8, !9, !10, !11, !12, !13, !14, !15, !16, !17, !18, !19, !20, !21, !22}

!0 = !{i32 2, i32 0, i32 0}
!1 = !{ptr @multi_exit, !2, !3}
!2 = !{}
!3 = !{!4, !5, !6, !7}
!4 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!5 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!6 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!7 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
!8 = !{ptr @irreducible, !2, !3}
!9 = !{ptr @pointer_phi, !2, !3}
!10 = !{ptr @wide_integer_0, !2, !3}
!11 = !{ptr @wide_phi, !2, !3}
!12 = !{ptr @wide_integer_1, !2, !3}
!13 = !{ptr @wide_integer_2, !2, !3}
!14 = !{ptr @wide_integer_3, !2, !3}
!15 = !{ptr @wide_div_3, !2, !3}
!16 = !{ptr @wide_div_4, !2, !3}
!17 = !{ptr @wide_div_5, !2, !3}
!18 = !{ptr @wide_div_6, !2, !3}
!19 = !{ptr @pointer_local_null, !2, !3}
!20 = !{ptr @wide_address, !2, !3}
!21 = !{ptr @wide_shared, !2, !3}
!22 = !{ptr @wide_vector, !2, !3}
