; SPDX-License-Identifier: BSD-2-Clause
; Authored AIR IR examples, not output captured from Apple's compiler.
; Assembled to real bitcode by the unified host build, then parsed by LLVM.
source_filename = "Shaders.air.ll"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v96:128:128-v128:128:128-n8:16:32"
target triple = "air64_v27-apple-ios18.5.0"

%Parameters = type { i32, i32, i32, i32, <3 x float>, float }
%Position = type { <4 x float> }
%Raster = type { <4 x float>, <4 x float>, <2 x float> }

define void @air_add(ptr addrspace(1) %input, ptr addrspace(1) %output, i32 %index) {
  %wide = zext i32 %index to i64
  %src = getelementptr inbounds i32, ptr addrspace(1) %input, i64 %wide
  %dst = getelementptr inbounds i32, ptr addrspace(1) %output, i64 %wide
  %value = load i32, ptr addrspace(1) %src, align 4
  %sum = add i32 %value, 7
  store i32 %sum, ptr addrspace(1) %dst, align 4
  ret void
}

define %Position @air_position(ptr addrspace(1) %vertices, i32 %index) {
  %wide = zext i32 %index to i64
  %src = getelementptr inbounds <4 x float>, ptr addrspace(1) %vertices, i64 %wide
  %value = load <4 x float>, ptr addrspace(1) %src, align 16
  %result = insertvalue %Position poison, <4 x float> %value, 0
  ret %Position %result
}

define <4 x float> @air_color() {
  ret <4 x float> <float 0.25, float 0.5, float 0.75, float 1.0>
}

define <4 x float> @air_textured(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <4 x float> %pixel) {
  %xy = shufflevector <4 x float> %pixel, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %uv = fdiv fast <2 x float> %xy, <float 64.0, float 64.0>
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  ret <4 x float> %color
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define %Position @air_instanced_position(ptr addrspace(1) %vertices, ptr addrspace(1) %offsets, i32 %vertex, i32 %instance) {
  %vertex_wide = zext i32 %vertex to i64
  %instance_wide = zext i32 %instance to i64
  %src = getelementptr inbounds <4 x float>, ptr addrspace(1) %vertices, i64 %vertex_wide
  %off = getelementptr inbounds <4 x float>, ptr addrspace(1) %offsets, i64 %instance_wide
  %position = load <4 x float>, ptr addrspace(1) %src, align 16
  %offset = load <4 x float>, ptr addrspace(1) %off, align 16
  %value = fadd fast <4 x float> %position, %offset
  %result = insertvalue %Position poison, <4 x float> %value, 0
  ret %Position %result
}

define %Raster @air_varying_position(ptr addrspace(1) %vertices, ptr addrspace(1) %colors, ptr addrspace(1) %coordinates, i32 %index) {
  %wide = zext i32 %index to i64
  %p = getelementptr inbounds <4 x float>, ptr addrspace(1) %vertices, i64 %wide
  %c = getelementptr inbounds <4 x float>, ptr addrspace(1) %colors, i64 %wide
  %t = getelementptr inbounds <4 x float>, ptr addrspace(1) %coordinates, i64 %wide
  %position = load <4 x float>, ptr addrspace(1) %p, align 16
  %color = load <4 x float>, ptr addrspace(1) %c, align 16
  %texcoord = load <4 x float>, ptr addrspace(1) %t, align 16
  %uv = shufflevector <4 x float> %texcoord, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %a = insertvalue %Raster poison, <4 x float> %position, 0
  %b = insertvalue %Raster %a, <4 x float> %color, 1
  %result = insertvalue %Raster %b, <2 x float> %uv, 2
  ret %Raster %result
}

define <4 x float> @air_varying_color(<2 x float> %uv, <4 x float> %color) {
  ret <4 x float> %color
}

define <4 x float> @air_varying_textured(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <2 x float> %uv, <4 x float> %tint) {
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %result = fmul fast <4 x float> %color, %tint
  ret <4 x float> %result
}


; Observe the same dispatch coordinates as the MSL entry.
define void @air_workgroup_coordinates(ptr addrspace(1) %output, <3 x i32> %gridID, <3 x i32> %localID, <3 x i32> %groupID, <3 x i32> %localSize, <3 x i32> %groupCount, <3 x i32> %gridSize, i32 %linearID) {
entry:
  %gridID0 = extractelement <3 x i32> %gridID, i32 0
  %gridID1 = extractelement <3 x i32> %gridID, i32 1
  %gridID2 = extractelement <3 x i32> %gridID, i32 2
  %localID0 = extractelement <3 x i32> %localID, i32 0
  %localID1 = extractelement <3 x i32> %localID, i32 1
  %localID2 = extractelement <3 x i32> %localID, i32 2
  %groupID0 = extractelement <3 x i32> %groupID, i32 0
  %groupID1 = extractelement <3 x i32> %groupID, i32 1
  %groupID2 = extractelement <3 x i32> %groupID, i32 2
  %localSize0 = extractelement <3 x i32> %localSize, i32 0
  %localSize1 = extractelement <3 x i32> %localSize, i32 1
  %localSize2 = extractelement <3 x i32> %localSize, i32 2
  %groupCount0 = extractelement <3 x i32> %groupCount, i32 0
  %groupCount1 = extractelement <3 x i32> %groupCount, i32 1
  %groupCount2 = extractelement <3 x i32> %groupCount, i32 2
  %gridSize0 = extractelement <3 x i32> %gridSize, i32 0
  %gridSize1 = extractelement <3 x i32> %gridSize, i32 1
  %gridSize2 = extractelement <3 x i32> %gridSize, i32 2
  %lastY = sub i32 %gridSize1, 1
  %lastZ = sub i32 %gridSize2, 1
  %skipY = icmp ne i32 %gridID1, %lastY
  %skipZ = icmp ne i32 %gridID2, %lastZ
  %skip = or i1 %skipY, %skipZ
  br i1 %skip, label %end, label %write
write:
  %term0 = mul i32 %gridID0, 1
  %term1 = mul i32 %gridID1, 3
  %sum1 = add i32 %term0, %term1
  %term2 = mul i32 %gridID2, 5
  %sum2 = add i32 %sum1, %term2
  %term3 = mul i32 %localID0, 7
  %sum3 = add i32 %sum2, %term3
  %term4 = mul i32 %localID1, 11
  %sum4 = add i32 %sum3, %term4
  %term5 = mul i32 %localID2, 13
  %sum5 = add i32 %sum4, %term5
  %term6 = mul i32 %groupID0, 17
  %sum6 = add i32 %sum5, %term6
  %term7 = mul i32 %groupID1, 19
  %sum7 = add i32 %sum6, %term7
  %term8 = mul i32 %groupID2, 23
  %sum8 = add i32 %sum7, %term8
  %term9 = mul i32 %localSize0, 29
  %sum9 = add i32 %sum8, %term9
  %term10 = mul i32 %localSize1, 31
  %sum10 = add i32 %sum9, %term10
  %term11 = mul i32 %localSize2, 37
  %sum11 = add i32 %sum10, %term11
  %term12 = mul i32 %groupCount0, 41
  %sum12 = add i32 %sum11, %term12
  %term13 = mul i32 %groupCount1, 43
  %sum13 = add i32 %sum12, %term13
  %term14 = mul i32 %groupCount2, 47
  %sum14 = add i32 %sum13, %term14
  %term15 = mul i32 %gridSize0, 53
  %sum15 = add i32 %sum14, %term15
  %term16 = mul i32 %gridSize1, 59
  %sum16 = add i32 %sum15, %term16
  %term17 = mul i32 %gridSize2, 61
  %sum17 = add i32 %sum16, %term17
  %term18 = mul i32 %linearID, 67
  %sum18 = add i32 %sum17, %term18
  %wide = zext i32 %gridID0 to i64
  %dst = getelementptr inbounds i32, ptr addrspace(1) %output, i64 %wide
  store i32 %sum18, ptr addrspace(1) %dst, align 4
  br label %end
end:
  ret void
}

define void @air_buffer_indexing(ptr addrspace(1) %input, ptr addrspace(1) %output, ptr addrspace(2) %parameters, <3 x i32> %id) {
entry:
  %x = extractelement <3 x i32> %id, i32 0
  %y = extractelement <3 x i32> %id, i32 1
  %z = extractelement <3 x i32> %id, i32 2
  %wp = getelementptr %Parameters, ptr addrspace(2) %parameters, i32 0, i32 0
  %hp = getelementptr %Parameters, ptr addrspace(2) %parameters, i32 0, i32 1
  %width = load i32, ptr addrspace(2) %wp, align 4
  %height = load i32, ptr addrspace(2) %hp, align 4
  %badx = icmp uge i32 %x, %width
  %bady = icmp uge i32 %y, %height
  %badz = icmp ne i32 %z, 0
  %badxy = or i1 %badx, %bady
  %bad = or i1 %badxy, %badz
  br i1 %bad, label %end, label %row
row:
  %rowbase = mul i32 %y, %width
  %index = add i32 %rowbase, %x
  %cp = getelementptr %Parameters, ptr addrspace(2) %parameters, i32 0, i32 3
  %count = load i32, ptr addrspace(2) %cp, align 4
  %outside = icmp uge i32 %index, %count
  br i1 %outside, label %end, label %write
write:
  %op = getelementptr %Parameters, ptr addrspace(2) %parameters, i32 0, i32 2
  %offset = load i32, ptr addrspace(2) %op, align 4
  %sourceindex = add i32 %offset, %index
  %sourcewide = zext i32 %sourceindex to i64
  %wide = zext i32 %index to i64
  %src = getelementptr i32, ptr addrspace(1) %input, i64 %sourcewide
  %dst = getelementptr i32, ptr addrspace(1) %output, i64 %wide
  %value = load i32, ptr addrspace(1) %src, align 4
  %tp = getelementptr %Parameters, ptr addrspace(2) %parameters, i32 0, i32 4, i32 1
  %gp = getelementptr %Parameters, ptr addrspace(2) %parameters, i32 0, i32 5
  %tint = load float, ptr addrspace(2) %tp, align 4
  %gain = load float, ptr addrspace(2) %gp, align 4
  %product = fmul fast float %tint, %gain
  %extra = fptoui float %product to i32
  %answer = add i32 %value, %extra
  store i32 %answer, ptr addrspace(1) %dst, align 4
  br label %end
end:
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
; This matches control_flow in Shaders.metal. The two loop PHIs exchange
; values on each backedge, so sequential PHI assignments would be incorrect.
define void @air_control_flow(ptr addrspace(1) %input, ptr addrspace(1) %output, i32 %index) {
entry:
  %wide = zext i32 %index to i64
  %src = getelementptr inbounds i32, ptr addrspace(1) %input, i64 %wide
  %dst = getelementptr inbounds i32, ptr addrspace(1) %output, i64 %wide
  %seed = load i32, ptr addrspace(1) %src, align 4
  %other = xor i32 %seed, 165
  %bits = and i32 %index, 7
  %limit = add i32 %bits, 1
  br label %loop
loop:
  %a = phi i32 [ %seed, %entry ], [ %b, %body ]
  %b = phi i32 [ %other, %entry ], [ %a, %body ]
  %n = phi i32 [ 0, %entry ], [ %next, %body ]
  %more = icmp ult i32 %n, %limit
  br i1 %more, label %body, label %choose
body:
  %next = add i32 %n, 1
  br label %loop
choose:
  %tag = and i32 %index, 3
  switch i32 %tag, label %otherwise [ i32 0, label %add
                                      i32 1, label %bitwise ]
add:
  %added = add i32 %a, 11
  br label %join
bitwise:
  %flipped = xor i32 %a, 17
  br label %join
otherwise:
  %combined = add i32 %a, %b
  br label %join
join:
  %answer = phi i32 [ %added, %add ], [ %flipped, %bitwise ], [ %combined, %otherwise ]
  %parity = and i32 %index, 1
  %even = icmp eq i32 %parity, 0
  br i1 %even, label %early, label %finish
early:
  store i32 %answer, ptr addrspace(1) %dst, align 4
  ret void
finish:
  %sum = add i32 %answer, %b
  store i32 %sum, ptr addrspace(1) %dst, align 4
  ret void
}

!air.kernel = !{!2, !53, !54, !64, !74, !79, !85, !90, !102,!108}
%AttributeRaster = type { <4 x float>, <4 x float> }
define %AttributeRaster @air_attribute_position(<4 x float> %position, <4 x float> %color, <2 x float> %offset) {
  %delta = shufflevector <2 x float> %offset, <2 x float> zeroinitializer, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %p = fadd fast <4 x float> %position, %delta
  %r0 = insertvalue %AttributeRaster poison, <4 x float> %p, 0
  %r1 = insertvalue %AttributeRaster %r0, <4 x float> %color, 1
  ret %AttributeRaster %r1
}
define <4 x float> @air_attribute_color(<4 x float> %color) { ret <4 x float> %color }

!air.vertex = !{!7, !22, !27, !43, !115, !122}
!air.fragment = !{!11, !14, !34, !39, !49, !68, !101, !128}
!0 = !{i32 2, i32 7, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @air_add, !3, !4}
!3 = !{}
!4 = !{!5, !6, !8}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!7 = !{ptr @air_position, !9, !10}
!8 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
!9 = !{!12}
!10 = !{!13, !15}
!11 = !{ptr @air_color, !16, !3}
!12 = !{!"air.position", !"air.arg_type_name", !"float4"}
!13 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!14 = !{ptr @air_textured, !16, !18}
!15 = !{i32 1, !"air.vertex_id", !"air.arg_type_name", !"uint"}
!16 = !{!17}
!17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!18 = !{!19, !20, !21}
!19 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>"}
!20 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler"}
!21 = !{i32 2, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4"}
!22 = !{ptr @air_instanced_position, !9, !23}
!23 = !{!13, !24, !25, !26}
!24 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!25 = !{i32 2, !"air.vertex_id", !"air.arg_type_name", !"uint"}
!26 = !{i32 3, !"air.instance_id", !"air.arg_type_name", !"uint"}
!27 = !{ptr @air_varying_position, !28, !31}
!28 = !{!12, !29, !30}
!29 = !{!"air.vertex_output", !"user(color)", !"air.arg_type_name", !"float4"}
!30 = !{!"air.vertex_output", !"user(texcoord)", !"air.arg_type_name", !"float2"}
!31 = !{!13, !24, !32, !33}
!32 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!33 = !{i32 3, !"air.vertex_id", !"air.arg_type_name", !"uint"}
!34 = !{ptr @air_varying_color, !16, !35}
!35 = !{!36, !37}
!36 = !{i32 0, !"air.fragment_input", !"user(texcoord)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2"}
!37 = !{i32 1, !"air.fragment_input", !"user(color)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4"}
!39 = !{ptr @air_varying_textured, !16, !40}
!40 = !{!19, !20, !41, !42}
!41 = !{i32 2, !"air.fragment_input", !"user(texcoord)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2"}
!42 = !{i32 3, !"air.fragment_input", !"user(color)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4"}

!43 = !{ptr @air_attribute_position, !44, !46}
!44 = !{!12, !45}
!45 = !{!"air.vertex_output", !"user(attribute_color)", !"air.arg_type_name", !"float4"}
!46 = !{!47, !48, !51}
!47 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float4"}
!48 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 3, i32 1, !"air.arg_type_name", !"float4"}
!49 = !{ptr @air_attribute_color, !16, !50}
!50 = !{!52}
!51 = !{i32 2, !"air.vertex_input", !"air.location_index", i32 7, i32 1, !"air.arg_type_name", !"float2"}
!52 = !{i32 0, !"air.fragment_input", !"user(attribute_color)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4"}
!53 = !{ptr @air_control_flow, !3, !4}

!54 = !{ptr @air_workgroup_coordinates, !3, !55}
!55 = !{!56, !57, !58, !59, !60, !61, !62, !63}
!56 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!57 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3"}
!58 = !{i32 2, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"uint3"}
!59 = !{i32 3, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint3"}
!60 = !{i32 4, !"air.threads_per_threadgroup", !"air.arg_type_name", !"uint3"}
!61 = !{i32 5, !"air.threadgroups_per_grid", !"air.arg_type_name", !"uint3"}
!62 = !{i32 6, !"air.threads_per_grid", !"air.arg_type_name", !"uint3"}
!63 = !{i32 7, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint"}

!64 = !{ptr @air_buffer_indexing, !3, !65}
!65 = !{!5, !6, !66, !67}
!66 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 48, !"air.arg_type_name", !"Parameters"}
!67 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3"}

%FragmentParameters = type { i32, i32, i32, i32, <4 x float> }
define <4 x float> @air_buffer_color(<4 x float> %position, ptr addrspace(1) %colors, ptr addrspace(2) %parameters, ptr addrspace(1) %output) {
  %px = extractelement <4 x float> %position, i32 0
  %py = extractelement <4 x float> %position, i32 1
  %x = fptoui float %px to i32
  %y = fptoui float %py to i32
  %widthp = getelementptr %FragmentParameters, ptr addrspace(2) %parameters, i32 0, i32 0
  %heightp = getelementptr %FragmentParameters, ptr addrspace(2) %parameters, i32 0, i32 1
  %width = load i32, ptr addrspace(2) %widthp, align 4
  %height = load i32, ptr addrspace(2) %heightp, align 4
  %xvalid = icmp ult i32 %x, %width
  %yvalid = icmp ult i32 %y, %height
  %valid = and i1 %xvalid, %yvalid
  br i1 %valid, label %shade, label %outside
outside:
  ret <4 x float> zeroinitializer
shade:
  %row = mul i32 %y, %width
  %index = add i32 %row, %x
  %outp = getelementptr %FragmentParameters, ptr addrspace(2) %parameters, i32 0, i32 2
  %palettep = getelementptr %FragmentParameters, ptr addrspace(2) %parameters, i32 0, i32 3
  %outoffset = load i32, ptr addrspace(2) %outp, align 4
  %paletteoffset = load i32, ptr addrspace(2) %palettep, align 4
  %outindex = add i32 %outoffset, %index
  %outaddr = getelementptr i32, ptr addrspace(1) %output, i32 %outindex
  %value = add i32 %index, 17
  store i32 %value, ptr addrspace(1) %outaddr, align 4
  %colorindex = add i32 %paletteoffset, %index
  %colorp = getelementptr <4 x float>, ptr addrspace(1) %colors, i32 %colorindex
  %color = load <4 x float>, ptr addrspace(1) %colorp, align 16
  %tintp = getelementptr %FragmentParameters, ptr addrspace(2) %parameters, i32 0, i32 4
  %tint = load <4 x float>, ptr addrspace(2) %tintp, align 16
  %result = fmul <4 x float> %color, %tint
  ret <4 x float> %result
}
!68 = !{ptr @air_buffer_color, !16, !69}
!69 = !{!70, !71, !72, !73}
!70 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4"}
!71 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!72 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 32, !"air.arg_type_name", !"FragmentParameters"}
!73 = !{i32 3, !"air.buffer", !"air.location_index", i32 7, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}

@air_scratch = internal addrspace(3) global [64 x i32] undef, align 16
declare void @air.wg.barrier(i32, i32) convergent nounwind
define void @air_threadgroup_reduce(ptr addrspace(1) %input, ptr addrspace(1) %output, i32 %lane, i32 %group, i32 %count) {
  %base = mul i32 %group, %count
  %index = add i32 %base, %lane
  %src = getelementptr i32, ptr addrspace(1) %input, i32 %index
  %value = load i32, ptr addrspace(1) %src, align 4
  %localp = getelementptr [64 x i32], ptr addrspace(3) @air_scratch, i32 0, i32 %lane
  store i32 %value, ptr addrspace(3) %localp, align 4
  call void @air.wg.barrier(i32 2, i32 1)
  %leader = icmp eq i32 %lane, 0
  br i1 %leader, label %loop, label %finish
loop:
  %n = phi i32 [0, %0], [%next, %body]
  %sum = phi i32 [0, %0], [%add, %body]
  %inrange = icmp ult i32 %n, %count
  %inshared = icmp ult i32 %n, 64
  %valid = and i1 %inrange, %inshared
  br i1 %valid, label %body, label %write
body:
  %readp = getelementptr [64 x i32], ptr addrspace(3) @air_scratch, i32 0, i32 %n
  %item = load i32, ptr addrspace(3) %readp, align 4
  %add = add i32 %sum, %item
  %next = add i32 %n, 1
  br label %loop
write:
  %dst = getelementptr i32, ptr addrspace(1) %output, i32 %group
  store i32 %sum, ptr addrspace(1) %dst, align 4
  br label %finish
finish:
  ret void
}
!74 = !{ptr @air_threadgroup_reduce, !3, !75}
!75 = !{!5, !6, !76, !77, !78}
!76 = !{i32 2, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint"}
!77 = !{i32 3, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint"}
!78 = !{i32 4, !"air.threads_per_threadgroup", !"air.arg_type_name", !"uint"}

@air_atomic_counter = internal addrspace(3) global i32 undef, align 4
declare void @air.atomic.local.store.i32(ptr addrspace(3), i32, i32, i32, i1)
declare i32 @air.atomic.local.add.u.i32(ptr addrspace(3), i32, i32, i32, i1)
declare i32 @air.atomic.local.load.i32(ptr addrspace(3), i32, i32, i1)
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i1)
define void @air_atomic_counter_kernel(ptr addrspace(1) %counter, ptr addrspace(1) %output, i32 %id, i32 %lane, i32 %group, i32 %groups, i32 %count) {
entry:
  %leader = icmp eq i32 %lane, 0
  br i1 %leader, label %initialize, label %counting
initialize:
  call void @air.atomic.local.store.i32(ptr addrspace(3) @air_atomic_counter, i32 0, i32 0, i32 1, i1 true)
  br label %counting
counting:
  call void @air.wg.barrier(i32 2, i32 1)
  %local = atomicrmw add ptr addrspace(3) @air_atomic_counter, i32 1 monotonic
  %ticket = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %counter, i32 1, i32 0, i32 2, i1 true)
  %twice = mul i32 %id, 2
  %next = add i32 %twice, 1
  %firstp = getelementptr i32, ptr addrspace(1) %output, i32 %twice
  %secondp = getelementptr i32, ptr addrspace(1) %output, i32 %next
  store i32 %ticket, ptr addrspace(1) %firstp, align 4
  store i32 %local, ptr addrspace(1) %secondp, align 4
  call void @air.wg.barrier(i32 2, i32 1)
  br i1 %leader, label %write, label %finish
write:
  %size = mul i32 %groups, %count
  %start = mul i32 %size, 2
  %at = add i32 %start, %group
  %dst = getelementptr i32, ptr addrspace(1) %output, i32 %at
  %total = call i32 @air.atomic.local.load.i32(ptr addrspace(3) @air_atomic_counter, i32 0, i32 1, i1 true)
  store i32 %total, ptr addrspace(1) %dst, align 4
  br label %finish
finish:
  ret void
}
!79 = !{ptr @air_atomic_counter_kernel, !3, !80}
!80 = !{!81, !6, !8, !82, !83, !84, !86}
!81 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"atomic_uint"}
!82 = !{i32 3, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint"}
!83 = !{i32 4, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint"}
!84 = !{i32 5, !"air.threadgroups_per_grid", !"air.arg_type_name", !"uint"}
!86 = !{i32 6, !"air.threads_per_threadgroup", !"air.arg_type_name", !"uint"}

declare void @air.atomic.global.store.i32(ptr addrspace(1), i32, i32, i32, i1)
declare i32 @air.atomic.global.load.i32(ptr addrspace(1), i32, i32, i1)
declare i32 @air.atomic.global.min.s.i32(ptr addrspace(1), i32, i32, i32, i1)
declare i32 @air.atomic.global.max.s.i32(ptr addrspace(1), i32, i32, i32, i1)
declare i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), ptr, i32, i32, i32, i32, i1)
define void @air_atomic_values(ptr addrspace(1) %values, ptr addrspace(1) %output, i32 %id) {
  %expected = alloca i32, align 4
  %pointer = getelementptr i32, ptr addrspace(1) %values, i32 %id
  %initial = add i32 %id, 16
  store atomic i32 %initial, ptr addrspace(1) %pointer monotonic, align 4
  %minimum = atomicrmw min ptr addrspace(1) %pointer, i32 -7 monotonic
  %maximum = call i32 @air.atomic.global.max.s.i32(ptr addrspace(1) %pointer, i32 -3, i32 0, i32 2, i1 true)
  store i32 0, ptr %expected, align 4
  %failedOld = call i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %pointer, ptr %expected, i32 9, i32 0, i32 0, i32 2, i1 true)
  %failed = icmp eq i32 %failedOld, 0
  %wanted = load i32, ptr %expected, align 4
  %desired = add i32 %id, 4
  %successPair = cmpxchg ptr addrspace(1) %pointer, i32 %wanted, i32 %desired monotonic monotonic
  %successOld = extractvalue {i32, i1} %successPair, 0
  %success = extractvalue {i32, i1} %successPair, 1
  store i32 %successOld, ptr %expected, align 4
  %expectedValue = load i32, ptr %expected, align 4
  %final = load atomic i32, ptr addrspace(1) %pointer monotonic, align 4
  %failureBit = zext i1 %failed to i32
  %successBit = select i1 %success, i32 2, i32 0
  %bits = or i32 %failureBit, %successBit
  %base = mul i32 %id, 5
  %at1 = add i32 %base, 1
  %at2 = add i32 %base, 2
  %at3 = add i32 %base, 3
  %at4 = add i32 %base, 4
  %p0 = getelementptr i32, ptr addrspace(1) %output, i32 %base
  %p1 = getelementptr i32, ptr addrspace(1) %output, i32 %at1
  %p2 = getelementptr i32, ptr addrspace(1) %output, i32 %at2
  %p3 = getelementptr i32, ptr addrspace(1) %output, i32 %at3
  %p4 = getelementptr i32, ptr addrspace(1) %output, i32 %at4
  store i32 %minimum, ptr addrspace(1) %p0, align 4
  store i32 %maximum, ptr addrspace(1) %p1, align 4
  store i32 %expectedValue, ptr addrspace(1) %p2, align 4
  store i32 %final, ptr addrspace(1) %p3, align 4
  store i32 %bits, ptr addrspace(1) %p4, align 4
  ret void
}
!85 = !{ptr @air_atomic_values, !3, !87}
!87 = !{!88, !6, !8}
!88 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"atomic_int"}

@air_dynamic_seeds = internal addrspace(3) global [3 x i32] undef, align 4
define void @air_dynamic_threadgroup(ptr addrspace(1) %output, ptr addrspace(3) %scratch,
    ptr addrspace(3) %vectors, ptr addrspace(3) %counter, i32 %id, i32 %lane,
    i32 %group, i32 %groups, i32 %count) {
entry:
  %first = icmp eq i32 %lane, 0
  br i1 %first, label %init, label %work
init:
  %initial = add i32 %group, 11
  store i32 %initial, ptr addrspace(3) @air_dynamic_seeds, align 4
  store atomic i32 -7, ptr addrspace(3) %counter monotonic, align 4
  br label %work
work:
  call void @air.wg.barrier(i32 2, i32 1)
  %seed = load i32, ptr addrspace(3) @air_dynamic_seeds, align 4
  %base = mul i32 %id, 3
  %value0 = add i32 %base, %seed
  %value1 = add i32 %value0, %lane
  %value2 = add i32 %value1, %count
  %value = add i32 %value2, 5
  %wideLane = zext i32 %lane to i64
  %cell = getelementptr i32, ptr addrspace(3) %scratch, i64 %wideLane
  store i32 %value, ptr addrspace(3) %cell, align 4
  %ticket = atomicrmw add ptr addrspace(3) %counter, i32 1 monotonic
  %unsignedTicket = add i32 %ticket, 7
  %at0 = mul i32 %id, 2
  %at1 = add i32 %at0, 1
  %dst0 = getelementptr i32, ptr addrspace(1) %output, i32 %at0
  %dst1 = getelementptr i32, ptr addrspace(1) %output, i32 %at1
  store i32 %unsignedTicket, ptr addrspace(1) %dst1, align 4
  call void @air.wg.barrier(i32 2, i32 1)
  %next = add i32 %lane, 1
  %neighbor = urem i32 %next, %count
  %wideNeighbor = zext i32 %neighbor to i64
  %source = getelementptr i32, ptr addrspace(3) %scratch, i64 %wideNeighbor
  %result = load i32, ptr addrspace(3) %source, align 4
  store i32 %result, ptr addrspace(1) %dst0, align 4
  br i1 %first, label %write, label %finish
write:
  %size = mul i32 %groups, %count
  %tail = mul i32 %size, 2
  %tail0 = add i32 %tail, %group
  %counterDst = getelementptr i32, ptr addrspace(1) %output, i32 %tail0
  %final = load atomic i32, ptr addrspace(3) %counter monotonic, align 4
  store i32 %final, ptr addrspace(1) %counterDst, align 4
  br label %finish
finish:
  ret void
}
!90 = !{ptr @air_dynamic_threadgroup, !3, !91}
!91 = !{!92, !93, !94, !95, !96, !97, !98, !99, !100}
!92 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!93 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!94 = !{i32 2, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float3"}
!95 = !{i32 3, !"air.buffer", !"air.location_index", i32 7, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"atomic_int"}
!96 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
!97 = !{i32 5, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint"}
!98 = !{i32 6, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint"}
!99 = !{i32 7, !"air.threadgroups_per_grid", !"air.arg_type_name", !"uint"}
!100 = !{i32 8, !"air.threads_per_threadgroup", !"air.arg_type_name", !"uint"}

define <4 x float> @air_mip_textured(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <4 x float> %pixel) {
  %x = extractelement <4 x float> %pixel, i32 0
  %y = extractelement <4 x float> %pixel, i32 1
  %u = fmul float %x, 0.125
  %v = fmul float %y, 0.125
  %lod = fmul float %y, 0.25
  %uv0 = insertelement <2 x float> poison, float %u, i32 0
  %uv = insertelement <2 x float> %uv0, float %v, i32 1
  %sample = call {<4 x float>, i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 true, float %lod, float 0.0, i32 0)
  %color = extractvalue {<4 x float>, i8} %sample, 0
  ret <4 x float> %color
}
!101 = !{ptr @air_mip_textured, !16, !18}

declare i32 @air.get_width_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_2d(ptr addrspace(1), i32)
declare {<4 x float>, i8} @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32)
declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i32, i32)
define void @air_image_transform(ptr addrspace(1) %input, ptr addrspace(1) %output,
    ptr addrspace(2) %parameters, <2 x i32> %id) {
entry:
  %level = load i32, ptr addrspace(2) %parameters, align 4
  %x = extractelement <2 x i32> %id, i32 0
  %y = extractelement <2 x i32> %id, i32 1
  %width = call i32 @air.get_width_texture_2d(ptr addrspace(1) %output, i32 %level)
  %height = call i32 @air.get_height_texture_2d(ptr addrspace(1) %output, i32 %level)
  %validx = icmp ult i32 %x, %width
  %validy = icmp ult i32 %y, %height
  %valid = and i1 %validx, %validy
  br i1 %valid, label %work, label %finish
work:
  %sample = call {<4 x float>, i8} @air.read_texture_2d.v4f32(ptr addrspace(1) %input, <2 x i32> %id, i32 %level, i32 1)
  %color = extractvalue {<4 x float>, i8} %sample, 0
  %red = extractelement <4 x float> %color, i32 0
  %green = extractelement <4 x float> %color, i32 1
  %blue = extractelement <4 x float> %color, i32 2
  %alpha = extractelement <4 x float> %color, i32 3
  %inverse = fsub float 1.0, %red
  %v0 = insertelement <4 x float> poison, float %blue, i32 0
  %v1 = insertelement <4 x float> %v0, float %green, i32 1
  %v2 = insertelement <4 x float> %v1, float %inverse, i32 2
  %value = insertelement <4 x float> %v2, float %alpha, i32 3
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %output, <2 x i32> %id, <4 x float> %value, i32 %level, i32 2)
  br label %finish
finish:
  ret void
}
!102 = !{ptr @air_image_transform, !3, !103}
!103 = !{!104, !105, !106, !107}
!104 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>"}
!105 = !{i32 1, !"air.texture", !"air.location_index", i32 7, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>"}
!106 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!107 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2"}

define void @air_sample_texture(ptr addrspace(1) %source,ptr addrspace(2) %filtering,ptr addrspace(1) %destination,ptr addrspace(2) %lodptr,<2 x i32> %id) {
  %w=call i32 @air.get_width_texture_2d(ptr addrspace(1) %destination,i32 0)
  %h=call i32 @air.get_height_texture_2d(ptr addrspace(1) %destination,i32 0)
  %x=extractelement <2 x i32> %id,i32 0
  %y=extractelement <2 x i32> %id,i32 1
  %in_x=icmp ult i32 %x,%w
  %in_y=icmp ult i32 %y,%h
  %valid=and i1 %in_x,%in_y
  br i1 %valid,label %sample,label %done
sample:
  %wf=uitofp i32 %w to float
  %hf=uitofp i32 %h to float
  %size0=insertelement <2 x float> poison,float %wf,i32 0
  %size=insertelement <2 x float> %size0,float %hf,i32 1
  %xy=uitofp <2 x i32> %id to <2 x float>
  %center=fadd fast <2 x float> %xy,<float 0.5,float 0.5>
  %uv=fdiv fast <2 x float> %center,%size
  %lod=load float,ptr addrspace(2) %lodptr,align 4
  %sampled=call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %source,ptr addrspace(2) %filtering,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 true,float %lod,float 0.0,i32 0)
  %color=extractvalue {<4 x float>,i8} %sampled,0
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %destination,<2 x i32> %id,<4 x float> %color,i32 0,i32 2)
  br label %done
done:
  ret void
}

!108 = !{ptr @air_sample_texture,!3,!109}
!109 = !{!110,!111,!112,!113,!114}
!110 = !{i32 0,!"air.texture",!"air.location_index",i32 5,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!111 = !{i32 1,!"air.sampler",!"air.location_index",i32 2,i32 1,!"air.arg_type_name",!"sampler"}
!112 = !{i32 2,!"air.texture",!"air.location_index",i32 7,i32 1,!"air.write",!"air.arg_type_name",!"texture2d<float, write>"}
!113 = !{i32 3,!"air.buffer",!"air.location_index",i32 0,!"air.read",!"air.address_space",i32 2,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
!114 = !{i32 4,!"air.thread_position_in_grid",!"air.arg_type_name",!"uint2"}

; Uses the existing verified 2D sample signature in a vertex entry. This is
; authored ABI coverage, not an assertion that target Metal has run it.
define %Raster @air_sampled_vertex(ptr addrspace(1) %vertices,ptr addrspace(1) %image,ptr addrspace(2) %filtering,ptr addrspace(2) %lodptr,i32 %index) {
  %wide = zext i32 %index to i64
  %src = getelementptr inbounds <4 x float>,ptr addrspace(1) %vertices,i64 %wide
  %position = load <4 x float>,ptr addrspace(1) %src,align 16
  %xy = shufflevector <4 x float> %position,<4 x float> poison,<2 x i32> <i32 0,i32 1>
  %shift = fadd <2 x float> %xy,<float 1.0,float 1.0>
  %uv = fmul <2 x float> %shift,<float 0.5,float 0.5>
  %lod = load float,ptr addrspace(2) %lodptr,align 4
  %sample = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %image,ptr addrspace(2) %filtering,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 true,float %lod,float 0.0,i32 0)
  %tint = extractvalue {<4 x float>,i8} %sample,0
  %red = extractelement <4 x float> %tint,i32 0
  %height = fmul float %red,0.125
  %z = extractelement <4 x float> %position,i32 2
  %displacedZ = fadd float %z,%height
  %displacedPosition = insertelement <4 x float> %position,float %displacedZ,i32 2
  %a = insertvalue %Raster poison,<4 x float> %displacedPosition,0
  %b = insertvalue %Raster %a,<4 x float> %tint,1
  %c = insertvalue %Raster %b,<2 x float> %uv,2
  ret %Raster %c
}
!115 = !{ptr @air_sampled_vertex,!28,!116}
!116 = !{!117,!118,!119,!120,!121}
!117 = !{i32 0,!"air.buffer",!"air.location_index",i32 0,!"air.read",!"air.address_space",i32 1,!"air.arg_type_size",i32 16,!"air.arg_type_name",!"float4"}
!118 = !{i32 1,!"air.texture",!"air.location_index",i32 0,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!119 = !{i32 2,!"air.sampler",!"air.location_index",i32 0,i32 1,!"air.arg_type_name",!"sampler"}
!120 = !{i32 3,!"air.buffer",!"air.location_index",i32 1,!"air.read",!"air.address_space",i32 2,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
!121 = !{i32 4,!"air.vertex_id",!"air.arg_type_name",!"uint"}

define %Raster @air_multitextured_vertex(ptr addrspace(1) %vertices,ptr addrspace(1) %image,ptr addrspace(2) %filtering,ptr addrspace(2) %lodptr,i32 %index,ptr addrspace(1) %other,ptr addrspace(2) %detail) {
  %wide = zext i32 %index to i64
  %src = getelementptr inbounds <4 x float>,ptr addrspace(1) %vertices,i64 %wide
  %position = load <4 x float>,ptr addrspace(1) %src,align 16
  %xy = shufflevector <4 x float> %position,<4 x float> poison,<2 x i32> <i32 0,i32 1>
  %shift = fadd <2 x float> %xy,<float 1.0,float 1.0>
  %uv = fmul <2 x float> %shift,<float 0.5,float 0.5>
  %lod = load float,ptr addrspace(2) %lodptr,align 4
  %sample = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %image,ptr addrspace(2) %filtering,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 true,float %lod,float 0.0,i32 0)
  %first = extractvalue {<4 x float>,i8} %sample,0
  %sample1 = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %image,ptr addrspace(2) %detail,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 false,float 0.0,float 0.0,i32 0)
  %second = extractvalue {<4 x float>,i8} %sample1,0
  %sample2 = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %other,ptr addrspace(2) %filtering,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 true,float %lod,float 0.0,i32 0)
  %third = extractvalue {<4 x float>,i8} %sample2,0
  %sum = fadd <4 x float> %first,%second
  %total = fadd <4 x float> %sum,%third
  %tint = fmul <4 x float> %total,<float 0.25,float 0.25,float 0.25,float 0.25>
  %red = extractelement <4 x float> %tint,i32 0
  %height = fmul float %red,0.125
  %z = extractelement <4 x float> %position,i32 2
  %displacedZ = fadd float %z,%height
  %displacedPosition = insertelement <4 x float> %position,float %displacedZ,i32 2
  %a = insertvalue %Raster poison,<4 x float> %displacedPosition,0
  %b = insertvalue %Raster %a,<4 x float> %tint,1
  %c = insertvalue %Raster %b,<2 x float> %uv,2
  ret %Raster %c
}
define <4 x float> @air_multitextured_fragment(ptr addrspace(1) %image,ptr addrspace(2) %filtering,<2 x float> %uv,<4 x float> %tint,ptr addrspace(1) %other,ptr addrspace(2) %detail,ptr addrspace(2) %lodptr) {
  %lod = load float,ptr addrspace(2) %lodptr,align 4
  %a = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %image,ptr addrspace(2) %filtering,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 false,float 0.0,float 0.0,i32 0)
  %b = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %image,ptr addrspace(2) %detail,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 true,float %lod,float 0.0,i32 0)
  %c = call {<4 x float>,i8} @air.sample_texture_2d.v4f32(ptr addrspace(1) %other,ptr addrspace(2) %filtering,<2 x float> %uv,i1 true,<2 x i32> zeroinitializer,i1 true,float %lod,float 0.0,i32 0)
  %first = extractvalue {<4 x float>,i8} %a,0
  %second = extractvalue {<4 x float>,i8} %b,0
  %third = extractvalue {<4 x float>,i8} %c,0
  %sum = fadd <4 x float> %first,%second
  %total = fadd <4 x float> %sum,%third
  %color = fmul <4 x float> %total,%tint
  %result = fmul <4 x float> %color,<float 0.25,float 0.25,float 0.25,float 0.25>
  ret <4 x float> %result
}
!122 = !{ptr @air_multitextured_vertex,!28,!123}
!123 = !{!117,!124,!125,!120,!121,!126,!127}
!124 = !{i32 1,!"air.texture",!"air.location_index",i32 6,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!125 = !{i32 2,!"air.sampler",!"air.location_index",i32 7,i32 1,!"air.arg_type_name",!"sampler"}
!126 = !{i32 5,!"air.texture",!"air.location_index",i32 1,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!127 = !{i32 6,!"air.sampler",!"air.location_index",i32 2,i32 1,!"air.arg_type_name",!"sampler"}
!128 = !{ptr @air_multitextured_fragment,!16,!129}
!129 = !{!130,!131,!41,!42,!132,!133,!134}
!130 = !{i32 0,!"air.texture",!"air.location_index",i32 6,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!131 = !{i32 1,!"air.sampler",!"air.location_index",i32 7,i32 1,!"air.arg_type_name",!"sampler"}
!132 = !{i32 4,!"air.texture",!"air.location_index",i32 1,i32 1,!"air.sample",!"air.arg_type_name",!"texture2d<float, sample>"}
!133 = !{i32 5,!"air.sampler",!"air.location_index",i32 2,i32 1,!"air.arg_type_name",!"sampler"}
!134 = !{i32 6,!"air.buffer",!"air.location_index",i32 0,!"air.read",!"air.address_space",i32 2,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
