; SPDX-License-Identifier: BSD-2-Clause
; Signed i1 conversion and splat constants must retain their LLVM semantics.
target datalayout = "e-p:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-n8:16:32"
target triple = "air64-apple-ios18.0.0"
define void @signed_bool(ptr addrspace(1) %output, i32 %id) {
 %bit = and i32 %id, 1
 %condition = icmp ne i32 %bit, 0
 %signed = sitofp i1 %condition to float
 %unsigned = uitofp i1 %condition to float
 %wide = sext i1 %condition to i32
 %slot = mul i32 %id, 4
 %a = getelementptr float, ptr addrspace(1) %output, i32 %slot
 store float %signed, ptr addrspace(1) %a
 %b = getelementptr float, ptr addrspace(1) %a, i32 1
 store float %unsigned, ptr addrspace(1) %b
 %c = getelementptr i32, ptr addrspace(1) %a, i32 2
 store i32 %wide, ptr addrspace(1) %c
 ret void
}
define void @nested_loops(ptr addrspace(1) %output, i32 %id) {
entry:
 br label %outer
outer:
 %i = phi i32 [0, %entry], [%inext, %outer_next]
 %sum = phi i32 [0, %entry], [%acc, %outer_next]
 %outer_more = icmp ult i32 %i, 4
 br i1 %outer_more, label %inner, label %done
inner:
 %j = phi i32 [0, %outer], [%jnext, %inner_next]
 %acc = phi i32 [%sum, %outer], [%newsum, %inner_next]
 %inner_more = icmp ult i32 %j, 3
 br i1 %inner_more, label %choose, label %outer_next
choose:
 %odd = and i32 %id, 1
 %one = icmp eq i32 %j, %odd
 br i1 %one, label %skip, label %add
skip:
 br label %inner_next
add:
 %v0 = add i32 %id, %i
 %v1 = add i32 %v0, %j
 %v2 = add i32 %acc, %v1
 br label %inner_next
inner_next:
 %newsum = phi i32 [%acc, %skip], [%v2, %add]
 %jnext = add i32 %j, 1
 br label %inner
outer_next:
 %inext = add i32 %i, 1
 br label %outer
done:
 %dest = getelementptr i32, ptr addrspace(1) %output, i32 %id
 store i32 %sum, ptr addrspace(1) %dest
 ret void
}
define <4 x float> @splat_color(<4 x float> %position) {
 %x = extractelement <4 x float> %position, i32 0
 %condition = fcmp olt float %x, 1.0
 br i1 %condition, label %zero, label %one
zero:
 ret <4 x float> zeroinitializer
one:
 ret <4 x float> <float 1.0, float 1.0, float 1.0, float 1.0>
}
declare float @air.exp.f32(float)
declare float @air.log.f32(float)
declare float @air.powr.f32(float,float)
declare float @air.atan2.f32(float,float)
declare float @air.fmin3.f32(float,float,float)
declare float @air.fmax3.f32(float,float,float)
define <4 x float> @extra_math(<4 x float> %position) {
 %x0 = extractelement <4 x float> %position, i32 0
 %x1 = fmul float %x0, 0.125
 %x = fadd float %x1, 1.0
 %y = extractelement <4 x float> %position, i32 1
 %e = call float @air.exp.f32(float %x1)
 %l = call float @air.log.f32(float %x)
 %p = call float @air.powr.f32(float %x,float 2.0)
 %a = call float @air.atan2.f32(float %y,float %x)
 %nan = extractelement <4 x float> %position, i32 2
 %min = call float @air.fmin3.f32(float %p,float %nan,float %e)
 %max = call float @air.fmax3.f32(float %p,float %e,float %nan)
 %r0 = insertelement <4 x float> poison,float %l,i32 0
 %r1 = insertelement <4 x float> %r0,float %a,i32 1
 %r2 = insertelement <4 x float> %r1,float %min,i32 2
 %r3 = insertelement <4 x float> %r2,float %max,i32 3
 ret <4 x float> %r3
}
!air.version = !{!0}
!air.kernel = !{!1,!12}
!air.fragment = !{!6,!10}
!0 = !{i32 2,i32 0,i32 0}
!1 = !{ptr @signed_bool,!2,!3}
!2 = !{}
!3 = !{!4,!5}
!4 = !{i32 0,!"air.buffer",!"air.location_index",i32 0,i32 1,!"air.read_write",!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
!5 = !{i32 1,!"air.thread_position_in_grid",!"air.arg_type_name",!"uint"}
!6 = !{ptr @splat_color,!7,!8}
!7 = !{!9}
!8 = !{!11}
!9 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}
!10 = !{ptr @extra_math,!7,!8}
!11 = !{i32 0,!"air.position",!"air.arg_type_name",!"float4"}

!12 = !{ptr @nested_loops,!2,!3}
