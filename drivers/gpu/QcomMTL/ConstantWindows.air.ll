; SPDX-License-Identifier: BSD-2-Clause
target datalayout = "e-p:64:64-i16:16-i32:32-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
define <4 x half> @constant_window_half(ptr addrspace(2) %input) {
 %p0 = getelementptr i8, ptr addrspace(2) %input, i64 176
 %p1 = getelementptr i8, ptr addrspace(2) %input, i64 184
 %p2 = getelementptr i8, ptr addrspace(2) %input, i64 192
 %p3 = getelementptr i8, ptr addrspace(2) %input, i64 200
 %p4 = getelementptr i8, ptr addrspace(2) %input, i64 208
 %v0 = load <4 x half>, ptr addrspace(2) %p0, align 2
 %v1 = load <4 x half>, ptr addrspace(2) %p1, align 2
 %v2 = load <4 x half>, ptr addrspace(2) %p2, align 2
 %v3 = load <4 x half>, ptr addrspace(2) %p3, align 2
 %v4 = load <4 x half>, ptr addrspace(2) %p4, align 2
 %s0 = fadd <4 x half> %v0, %v1
 %s1 = fadd <4 x half> %s0, %v2
 %s2 = fadd <4 x half> %s1, %v3
 %s3 = fadd <4 x half> %s2, %v4
 ret <4 x half> %s3
}
!air.version = !{!0}
!air.fragment = !{!1}
!0 = !{i32 2, i32 7, i32 0}
!1 = !{ptr @constant_window_half, !2, !3}
!2 = !{!4}
!3 = !{!5}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4"}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 2, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"half", !"air.arg_name", !"input"}
