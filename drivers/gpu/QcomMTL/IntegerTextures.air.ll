; SPDX-License-Identifier: BSD-2-Clause
; Authored uint image example, including the packed BGRA write used by VTMTS.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"

declare { <4 x i32>, i8 } @air.read_texture_2d.u.v4i32(ptr addrspace(1), <2 x i32>, i32, i32)
declare void @air.write_texture_2d.u.v4i32(ptr addrspace(1), <2 x i32>, <4 x i32>, i32, i32)
declare i32 @air.pack.unorm4x8.v4f16(<4 x half>)

define void @air_uint_copy(ptr addrspace(1) %input, ptr addrspace(1) %output, <2 x i32> %id) {
  %result = call { <4 x i32>, i8 } @air.read_texture_2d.u.v4i32(ptr addrspace(1) %input, <2 x i32> %id, i32 0, i32 1)
  %value = extractvalue { <4 x i32>, i8 } %result, 0
  %changed = xor <4 x i32> %value, <i32 -2147483647, i32 0, i32 0, i32 0>
  call void @air.write_texture_2d.u.v4i32(ptr addrspace(1) %output, <2 x i32> %id, <4 x i32> %changed, i32 0, i32 2)
  ret void
}

define void @air_half_bits(ptr addrspace(1) %input, ptr addrspace(1) %output, i32 %id) {
  %src = getelementptr <2 x i32>, ptr addrspace(1) %input, i32 %id
  %word = load <2 x i32>, ptr addrspace(1) %src, align 8
  %half = bitcast <2 x i32> %word to <4 x half>
  %swapped = shufflevector <4 x half> %half, <4 x half> poison, <4 x i32> <i32 1, i32 0, i32 3, i32 2>
  %bits = bitcast <4 x half> %swapped to <2 x i32>
  %dst = getelementptr <2 x i32>, ptr addrspace(1) %output, i32 %id
  store <2 x i32> %bits, ptr addrspace(1) %dst, align 8
  ret void
}

define void @air_private_array(ptr addrspace(1) %output, i32 %id) {
entry:
  %array = alloca [8 x <4 x i32>], align 16
  br label %loop
loop:
  %index = phi i32 [0, %entry], [%next, %loop]
  %value = xor i32 %id, %index
  %base = insertelement <4 x i32> poison, i32 %value, i32 0
  %words = shufflevector <4 x i32> %base, <4 x i32> poison, <4 x i32> zeroinitializer
  %where = getelementptr [8 x <4 x i32>], ptr %array, i32 0, i32 %index
  store <4 x i32> %words, ptr %where, align 16
  %next = add i32 %index, 1
  %again = icmp ult i32 %next, 8
  br i1 %again, label %loop, label %done
done:
  %selected = and i32 %id, 7
  %cell = getelementptr [8 x <4 x i32>], ptr %array, i32 0, i32 %selected
  %read = load <4 x i32>, ptr %cell, align 16
  %dst = getelementptr <4 x i32>, ptr addrspace(1) %output, i32 %id
  store <4 x i32> %read, ptr addrspace(1) %dst, align 16
  ret void
}

define void @air_uint_pack(ptr addrspace(1) %input, ptr addrspace(1) %output, <2 x i32> %id) {
  %rgba = load <4 x float>, ptr addrspace(1) %input, align 16
  %half = fptrunc <4 x float> %rgba to <4 x half>
  %packed = call i32 @air.pack.unorm4x8.v4f16(<4 x half> %half)
  %first = insertelement <4 x i32> poison, i32 %packed, i32 0
  %value = shufflevector <4 x i32> %first, <4 x i32> poison, <4 x i32> zeroinitializer
  call void @air.write_texture_2d.u.v4i32(ptr addrspace(1) %output, <2 x i32> %id, <4 x i32> %value, i32 0, i32 2)
  ret void
}

!air.kernel = !{!0, !10, !20, !30}
!air.version = !{!6}
!0 = !{ptr @air_uint_copy, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, !"air.read", !"air.arg_type_name", !"texture2d<uint, read>"}
!4 = !{i32 1, !"air.texture", !"air.location_index", i32 7, !"air.write", !"air.arg_type_name", !"texture2d<uint, write>"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2"}
!6 = !{i32 2, i32 7, i32 0}
!10 = !{ptr @air_half_bits, !1, !11}
!11 = !{!12, !13, !14}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_name", !"uint2"}
!13 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_name", !"uint2"}
!14 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
!20 = !{ptr @air_private_array, !1, !21}
!21 = !{!22, !23}
!22 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"uint4"}
!23 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
!30 = !{ptr @air_uint_pack, !1, !31}
!31 = !{!32, !33, !5}
!32 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!33 = !{i32 1, !"air.texture", !"air.location_index", i32 1, !"air.write", !"air.arg_type_name", !"texture2d<uint, write>"}
