; SPDX-License-Identifier: BSD-2-Clause
target datalayout = "e-p:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-n8:16:32"
target triple = "air64-apple-ios18.0.0"
@positions = internal addrspace(2) constant [3 x <2 x float>] [<2 x float> <float -1.0,float -1.0>,<2 x float> <float 1.0,float -1.0>,<2 x float> <float 0.0,float 1.0>]
declare float @air.fast_sincos.f32(float,ptr)
define void @packed_fields(ptr addrspace(1) %input,ptr addrspace(1) %output,i32 %index) {
entry:
 %cosine = alloca float,align 4
 %alias = bitcast ptr %cosine to ptr
 %byteptr = getelementptr i8,ptr addrspace(1) %input,i64 1
 %byte = load i8,ptr addrspace(1) %byteptr
 %wrapped = add i8 %byte,1
 %bytewide = zext i8 %wrapped to i32
 %shortptr = getelementptr i8,ptr addrspace(1) %input,i64 3
 %short = load i16,ptr addrspace(1) %shortptr,align 1
 %signed = sext i16 %short to i32
 %negative = icmp slt i16 %short,0
 %selected = select i1 %negative,i32 %signed,i32 %bytewide
 %angleptr = getelementptr i8,ptr addrspace(1) %input,i64 4
 %angle = load float,ptr addrspace(1) %angleptr
 %sine = call float @air.fast_sincos.f32(float %angle,ptr %alias)
 %cos = load float,ptr %cosine
 switch i16 %short,label %other [i16 -32768,label %minimum i16 32767,label %maximum]
minimum:
 br label %merge
maximum:
 br label %merge
other:
 br label %merge
merge:
 %tag = phi i32 [1,%minimum],[2,%maximum],[3,%other]
 store i32 %selected,ptr addrspace(1) %output
 %out1 = getelementptr i32,ptr addrspace(1) %output,i64 1
 store i32 %tag,ptr addrspace(1) %out1
 %out2 = getelementptr float,ptr addrspace(1) %output,i64 2
 store float %sine,ptr addrspace(1) %out2
 %out3 = getelementptr float,ptr addrspace(1) %output,i64 3
 store float %cos,ptr addrspace(1) %out3
 ret void
}
define <{<4 x float>}> @quad_table(i16 %vertex) {
 %wide = zext i16 %vertex to i64
 %ptr = getelementptr [3 x <2 x float>],ptr addrspace(2) @positions,i64 0,i64 %wide
 %xy = load <2 x float>,ptr addrspace(2) %ptr
 %position = shufflevector <2 x float> %xy,<2 x float> <float 0.0,float 1.0>,<4 x i32> <i32 0,i32 1,i32 2,i32 3>
 %result = insertvalue <{<4 x float>}> undef,<4 x float> %position,0
 ret <{<4 x float>}> %result
}
!air.version = !{!0}
!air.kernel = !{!1}
!air.vertex = !{!7}
!0 = !{i32 2,i32 0,i32 0}
!1 = !{ptr @packed_fields,!2,!3}
!2 = !{}
!3 = !{!4,!5,!6}
!4 = !{i32 0,!"air.buffer",!"air.location_index",i32 0,i32 1,!"air.read",!"air.arg_type_size",i32 4,!"air.arg_type_name",!"uint"}
!5 = !{i32 1,!"air.buffer",!"air.location_index",i32 1,i32 1,!"air.read_write",!"air.arg_type_size",i32 4,!"air.arg_type_name",!"uint"}
!6 = !{i32 2,!"air.thread_position_in_grid",!"air.arg_type_name",!"uint"}
!7 = !{ptr @quad_table,!8,!10}
!8 = !{!9}
!9 = !{!"air.position",!"air.arg_type_name",!"float4"}
!10 = !{!11}
!11 = !{i32 0,!"air.vertex_id",!"air.arg_type_name",!"ushort"}
