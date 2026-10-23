; SPDX-License-Identifier: BSD-2-Clause
; Authored AIR examples. Signatures follow the pinned public Apple outputs
; documented in tools/gpu-compiler/Source.json; these bodies are not captures.
; Kept separate because legacy/current reads share names but differ in type.
source_filename = "Textures.air.ll"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v96:128:128-v128:128:128-n8:16:32"
target triple = "air64_v27-apple-ios18.5.0"

%Raster = type { <4 x float>, <4 x float>, <2 x float> }
%Colors = type { <4 x float>, <4 x float>, <4 x float> }

@constant_filtering = internal addrspace(2) constant [2 x i64] [i64 19140521758313033, i64 0], align 8
@legacy_filtering = internal addrspace(2) constant i64 9242512558613088841, align 8

declare ptr addrspace(2) @air.get_read_sampler()
declare float @air.convert.f.f32.u.i32(i32)
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32)
declare { <4 x float>, i8 } @air.read_texture_2d_array.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, i32, <2 x i32>, i32, i32)
declare { <4 x float>, i8 } @air.read_texture_cube.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, i32, i32, i32)
declare { <4 x float>, i8 } @air.read_texture_cube_array.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, i32, i32, i32, i32)
declare void @air.write_texture_2d_array.v4f32(ptr addrspace(1), <2 x i32>, i32, <4 x float>, i32, i32)
declare void @air.write_texture_cube.v4f32(ptr addrspace(1), <2 x i32>, i32, <4 x float>, i32, i32)
declare i32 @air.get_width_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_num_mip_levels_texture_2d(ptr addrspace(1))
declare i32 @air.get_width_texture_2d_array(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_2d_array(ptr addrspace(1), i32)
declare i32 @air.get_num_mip_levels_texture_2d_array(ptr addrspace(1))
declare i32 @air.get_array_size_texture_2d_array(ptr addrspace(1))
declare i32 @air.get_width_texture_cube(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_cube(ptr addrspace(1), i32)
declare i32 @air.get_num_mip_levels_texture_cube(ptr addrspace(1))
declare i32 @air.get_width_texture_cube_array(ptr addrspace(1), i32)
declare i32 @air.get_array_size_texture_cube_array(ptr addrspace(1))
declare i32 @air.get_num_mip_levels_texture_cube_array(ptr addrspace(1))
declare { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i32, i1, <2 x i32>, i1, float, float, i32)
declare { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1), ptr addrspace(2), <3 x float>, i1, float, float, i32)
declare { <4 x float>, i8 } @air.sample_texture_cube_array_grad.v4f32(ptr addrspace(1), ptr addrspace(2), <3 x float>, i32, <3 x float>, <3 x float>, float, i32)

define %Raster @air_fetch_vertex(ptr addrspace(1) %vertices, ptr addrspace(1) %image, ptr addrspace(2) %controlPtr, i32 %index) {
  %wide = zext i32 %index to i64
  %src = getelementptr <4 x float>, ptr addrspace(1) %vertices, i64 %wide
  %position = load <4 x float>, ptr addrspace(1) %src, align 16
  %control = load <4 x i32>, ptr addrspace(2) %controlPtr, align 16
  %coord = shufflevector <4 x i32> %control, <4 x i32> poison, <2 x i32> <i32 0, i32 1>
  %lod = extractelement <4 x i32> %control, i32 3
  %rs = call ptr addrspace(2) @air.get_read_sampler()
  %sample = call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %rs, <2 x i32> %coord, <2 x i32> zeroinitializer, i32 %lod, i32 1)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %width = call i32 @air.get_width_texture_2d(ptr addrspace(1) %image, i32 %lod)
  %height = call i32 @air.get_height_texture_2d(ptr addrspace(1) %image, i32 %lod)
  %levels = call i32 @air.get_num_mip_levels_texture_2d(ptr addrspace(1) %image)
  %size = add i32 %width, %height
  %total = add i32 %size, %levels
  %denominator = add i32 %total, 1
  %scale = uitofp i32 %denominator to float
  %red = extractelement <4 x float> %color, i32 0
  %displacement = fdiv float %red, %scale
  %z = extractelement <4 x float> %position, i32 2
  %moved = fadd float %z, %displacement
  %p = insertelement <4 x float> %position, float %moved, i32 2
  %uv = shufflevector <4 x float> %position, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %a = insertvalue %Raster poison, <4 x float> %p, 0
  %b = insertvalue %Raster %a, <4 x float> %color, 1
  %out = insertvalue %Raster %b, <2 x float> %uv, 2
  ret %Raster %out
}

define <4 x float> @air_fetch_cube_fragment(ptr addrspace(1) %image, ptr addrspace(2) %controlPtr) {
  %control = load <4 x i32>, ptr addrspace(2) %controlPtr, align 16
  %coord = shufflevector <4 x i32> %control, <4 x i32> poison, <2 x i32> <i32 0, i32 1>
  %face = extractelement <4 x i32> %control, i32 2
  %lod = extractelement <4 x i32> %control, i32 3
  %rs = call ptr addrspace(2) @air.get_read_sampler()
  %sample = call { <4 x float>, i8 } @air.read_texture_cube.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %rs, <2 x i32> %coord, i32 %face, i32 %lod, i32 1)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %width = call i32 @air.get_width_texture_cube(ptr addrspace(1) %image, i32 %lod)
  %height = call i32 @air.get_height_texture_cube(ptr addrspace(1) %image, i32 %lod)
  %levels = call i32 @air.get_num_mip_levels_texture_cube(ptr addrspace(1) %image)
  %w = call float @air.convert.f.f32.u.i32(i32 %width)
  %h = call float @air.convert.f.f32.u.i32(i32 %height)
  %m = call float @air.convert.f.f32.u.i32(i32 %levels)
  %a = insertelement <4 x float> zeroinitializer, float %w, i32 0
  %b = insertelement <4 x float> %a, float %h, i32 1
  %c = insertelement <4 x float> %b, float %m, i32 2
  %info = fdiv <4 x float> %c, <float 65536.0, float 65536.0, float 65536.0, float 65536.0>
  %out = fadd <4 x float> %color, %info
  ret <4 x float> %out
}

define <4 x float> @air_fetch_cube_array_fragment(ptr addrspace(1) %image, ptr addrspace(2) %controlPtr, ptr addrspace(2) %lodPtr) {
  %control = load <4 x i32>, ptr addrspace(2) %controlPtr, align 16
  %coord = shufflevector <4 x i32> %control, <4 x i32> poison, <2 x i32> <i32 0, i32 1>
  %face = extractelement <4 x i32> %control, i32 2
  %cube = extractelement <4 x i32> %control, i32 3
  %lod = load i32, ptr addrspace(2) %lodPtr, align 4
  %rs = call ptr addrspace(2) @air.get_read_sampler()
  %sample = call { <4 x float>, i8 } @air.read_texture_cube_array.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %rs, <2 x i32> %coord, i32 %face, i32 %cube, i32 %lod, i32 1)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %cubes = call i32 @air.get_array_size_texture_cube_array(ptr addrspace(1) %image)
  %levels = call i32 @air.get_num_mip_levels_texture_cube_array(ptr addrspace(1) %image)
  %total = add i32 %cubes, %levels
  %value = uitofp i32 %total to float
  %scaled = fdiv float %value, 65536.0
  %a = insertelement <4 x float> zeroinitializer, float %scaled, i32 0
  %out = fadd <4 x float> %color, %a
  ret <4 x float> %out
}

define void @air_array_image_transform(ptr addrspace(1) %image, ptr addrspace(2) %lodPtr, <3 x i32> %id) {
  %coord = shufflevector <3 x i32> %id, <3 x i32> poison, <2 x i32> <i32 0, i32 1>
  %slice = extractelement <3 x i32> %id, i32 2
  %lod = load i32, ptr addrspace(2) %lodPtr, align 4
  %rs = call ptr addrspace(2) @air.get_read_sampler()
  %sample = call { <4 x float>, i8 } @air.read_texture_2d_array.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %rs, <2 x i32> %coord, i32 %slice, <2 x i32> zeroinitializer, i32 %lod, i32 3)
  ; Extraction must observe the original read, before this intermediate store.
  call void @air.write_texture_2d_array.v4f32(ptr addrspace(1) %image, <2 x i32> %coord, i32 %slice, <4 x float> zeroinitializer, i32 %lod, i32 3)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %levels = call i32 @air.get_num_mip_levels_texture_2d_array(ptr addrspace(1) %image)
  %layers = call i32 @air.get_array_size_texture_2d_array(ptr addrspace(1) %image)
  %total = add i32 %levels, %layers
  %one = add i32 %total, 1
  %scale = uitofp i32 %one to float
  %a = insertelement <4 x float> poison, float %scale, i32 0
  %splat = shufflevector <4 x float> %a, <4 x float> poison, <4 x i32> zeroinitializer
  %value = fdiv <4 x float> %color, %splat
  call void @air.write_texture_2d_array.v4f32(ptr addrspace(1) %image, <2 x i32> %coord, i32 %slice, <4 x float> %value, i32 %lod, i32 3)
  ret void
}

define void @air_cube_image_copy(ptr addrspace(1) %source, ptr addrspace(1) %destination, ptr addrspace(2) %lodPtr, <3 x i32> %id) {
  %coord = shufflevector <3 x i32> %id, <3 x i32> poison, <2 x i32> <i32 0, i32 1>
  %face = extractelement <3 x i32> %id, i32 2
  %lod = load i32, ptr addrspace(2) %lodPtr, align 4
  %rs = call ptr addrspace(2) @air.get_read_sampler()
  %sample = call { <4 x float>, i8 } @air.read_texture_cube.v4f32(ptr addrspace(1) %source, ptr addrspace(2) %rs, <2 x i32> %coord, i32 %face, i32 %lod, i32 1)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  call void @air.write_texture_cube.v4f32(ptr addrspace(1) %destination, <2 x i32> %coord, i32 %face, <4 x float> %color, i32 %lod, i32 2)
  ret void
}

define %Raster @air_cube_array_sample_vertex(ptr addrspace(1) %vertices, ptr addrspace(1) %image, ptr addrspace(2) %filtering, ptr addrspace(2) %directionPtr, ptr addrspace(2) %cubePtr, i32 %index) {
  %wide = zext i32 %index to i64
  %src = getelementptr <4 x float>, ptr addrspace(1) %vertices, i64 %wide
  %position = load <4 x float>, ptr addrspace(1) %src, align 16
  %direction4 = load <4 x float>, ptr addrspace(2) %directionPtr, align 16
  %direction = shufflevector <4 x float> %direction4, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %cube = load i32, ptr addrspace(2) %cubePtr, align 4
  %sample = call { <4 x float>, i8 } @air.sample_texture_cube_array_grad.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <3 x float> %direction, i32 %cube, <3 x float> <float 0.125, float 0.0, float 0.0>, <3 x float> <float 0.0, float 0.125, float 0.0>, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %width = call i32 @air.get_width_texture_cube_array(ptr addrspace(1) %image, i32 0)
  %cubes = call i32 @air.get_array_size_texture_cube_array(ptr addrspace(1) %image)
  %total = add i32 %width, %cubes
  %one = add i32 %total, 1
  %scale = uitofp i32 %one to float
  %red = extractelement <4 x float> %color, i32 0
  %displacement = fdiv float %red, %scale
  %z = extractelement <4 x float> %position, i32 2
  %moved = fadd float %z, %displacement
  %p = insertelement <4 x float> %position, float %moved, i32 2
  %uv = shufflevector <4 x float> %position, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %a = insertvalue %Raster poison, <4 x float> %p, 0
  %b = insertvalue %Raster %a, <4 x float> %color, 1
  %out = insertvalue %Raster %b, <2 x float> %uv, 2
  ret %Raster %out
}

define <4 x float> @air_array_sample_fragment(ptr addrspace(1) %image, ptr addrspace(2) %filtering, ptr addrspace(2) %controlPtr) {
  %control = load <2 x i32>, ptr addrspace(2) %controlPtr, align 8
  %slice = extractelement <2 x i32> %control, i32 0
  %lod = extractelement <2 x i32> %control, i32 1
  %level = uitofp i32 %lod to float
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <2 x float> <float 0.25, float 0.75>, i32 %slice, i1 false, <2 x i32> zeroinitializer, i1 true, float %level, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %height = call i32 @air.get_height_texture_2d_array(ptr addrspace(1) %image, i32 %lod)
  %layers = call i32 @air.get_array_size_texture_2d_array(ptr addrspace(1) %image)
  %total = add i32 %height, %layers
  %size = uitofp i32 %total to float
  %scaled = fdiv float %size, 65536.0
  %info = insertelement <4 x float> zeroinitializer, float %scaled, i32 0
  %out = fadd <4 x float> %color, %info
  ret <4 x float> %out
}

define <4 x float> @air_cube_sample_fragment(ptr addrspace(1) %image, ptr addrspace(2) %filtering, ptr addrspace(2) %directionPtr) {
  %direction4 = load <4 x float>, ptr addrspace(2) %directionPtr, align 16
  %direction = shufflevector <4 x float> %direction4, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %lod = extractelement <4 x float> %direction4, i32 3
  %sample = call { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %filtering, <3 x float> %direction, i1 true, float %lod, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %levels = call i32 @air.get_num_mip_levels_texture_cube(ptr addrspace(1) %image)
  %size = uitofp i32 %levels to float
  %scaled = fdiv float %size, 65536.0
  %info = insertelement <4 x float> zeroinitializer, float %scaled, i32 0
  %out = fadd <4 x float> %color, %info
  ret <4 x float> %out
}

define %Raster @air_constant_cube_vertex(ptr addrspace(1) %vertices, ptr addrspace(1) %image, ptr addrspace(2) %filtering, ptr addrspace(2) %directionPtr, ptr addrspace(2) %cubePtr, i32 %index) {
  %wide = zext i32 %index to i64
  %src = getelementptr <4 x float>, ptr addrspace(1) %vertices, i64 %wide
  %position = load <4 x float>, ptr addrspace(1) %src, align 16
  %direction4 = load <4 x float>, ptr addrspace(2) %directionPtr, align 16
  %direction = shufflevector <4 x float> %direction4, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %cube = load i32, ptr addrspace(2) %cubePtr, align 4
  %sample = call { <4 x float>, i8 } @air.sample_texture_cube_array_grad.v4f32(ptr addrspace(1) %image, ptr addrspace(2) @constant_filtering, <3 x float> %direction, i32 %cube, <3 x float> <float 0.125, float 0.0, float 0.0>, <3 x float> <float 0.0, float 0.125, float 0.0>, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %width = call i32 @air.get_width_texture_cube_array(ptr addrspace(1) %image, i32 0)
  %cubes = call i32 @air.get_array_size_texture_cube_array(ptr addrspace(1) %image)
  %total = add i32 %width, %cubes
  %one = add i32 %total, 1
  %scale = uitofp i32 %one to float
  %red = extractelement <4 x float> %color, i32 0
  %displacement = fdiv float %red, %scale
  %z = extractelement <4 x float> %position, i32 2
  %moved = fadd float %z, %displacement
  %p = insertelement <4 x float> %position, float %moved, i32 2
  %uv = shufflevector <4 x float> %position, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %a = insertvalue %Raster poison, <4 x float> %p, 0
  %b = insertvalue %Raster %a, <4 x float> %color, 1
  %out = insertvalue %Raster %b, <2 x float> %uv, 2
  ret %Raster %out
}

define <4 x float> @air_constant_array_fragment(ptr addrspace(1) %image, ptr addrspace(2) %filtering, ptr addrspace(2) %controlPtr) {
  %control = load <2 x i32>, ptr addrspace(2) %controlPtr, align 8
  %slice = extractelement <2 x i32> %control, i32 0
  %lod = extractelement <2 x i32> %control, i32 1
  %level = uitofp i32 %lod to float
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1) %image, ptr addrspace(2) @legacy_filtering, <2 x float> <float 0.25, float 0.75>, i32 %slice, i1 false, <2 x i32> zeroinitializer, i1 true, float %level, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %height = call i32 @air.get_height_texture_2d_array(ptr addrspace(1) %image, i32 %lod)
  %layers = call i32 @air.get_array_size_texture_2d_array(ptr addrspace(1) %image)
  %total = add i32 %height, %layers
  %size = uitofp i32 %total to float
  %scaled = fdiv float %size, 65536.0
  %info = insertelement <4 x float> zeroinitializer, float %scaled, i32 0
  %out = fadd <4 x float> %color, %info
  ret <4 x float> %out
}

define void @air_constant_sample(ptr addrspace(1) %out, ptr addrspace(1) %image, ptr addrspace(2) %coordinatePtr, i32 %index) {
  %coordinate = load <4 x float>, ptr addrspace(2) %coordinatePtr, align 16
  %uv = shufflevector <4 x float> %coordinate, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %lod = extractelement <4 x float> %coordinate, i32 3
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1) %image, ptr addrspace(2) @constant_filtering, <2 x float> %uv, i32 0, i1 false, <2 x i32> zeroinitializer, i1 true, float %lod, float 0.0, i32 0)
  %color = extractvalue { <4 x float>, i8 } %sample, 0
  %wide = zext i32 %index to i64
  %dst = getelementptr <4 x float>, ptr addrspace(1) %out, i64 %wide
  store <4 x float> %color, ptr addrspace(1) %dst, align 16
  ret void
}

define %Colors @air_mrt_fragment(ptr addrspace(2) %controlPtr, <4 x float> %position) {
  %control = load <4 x float>, ptr addrspace(2) %controlPtr, align 16
  %material = shufflevector <4 x float> %control, <4 x float> poison, <4 x i32> <i32 3, i32 2, i32 1, i32 0>
  %normal = fmul <4 x float> %position, <float 0.015625, float 0.015625, float 0.015625, float 0.015625>
  %a = insertvalue %Colors poison, <4 x float> %material, 0
  %b = insertvalue %Colors %a, <4 x float> %control, 1
  %out = insertvalue %Colors %b, <4 x float> %normal, 2
  ret %Colors %out
}
define <4 x float> @air_color_only_fragment(ptr addrspace(2) %controlPtr) {
  %value = load <4 x float>, ptr addrspace(2) %controlPtr, align 16
  ret <4 x float> %value
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!40, !45, !100}
!air.vertex = !{!10, !50, !80}
!air.fragment = !{!20, !30, !60, !70, !90, !110, !120, !130, !135, !140, !150}
!0 = !{i32 2, i32 6, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{}
!3 = !{!4, !5, !6}
!4 = !{!"air.position", !"air.arg_type_name", !"float4"}
!5 = !{!"air.vertex_output", !"user(color)", !"air.arg_type_name", !"float4"}
!6 = !{!"air.vertex_output", !"user(texcoord)", !"air.arg_type_name", !"float2"}
!7 = !{!8}
!8 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!10 = !{ptr @air_fetch_vertex, !3, !11}
!11 = !{!12, !13, !14, !15}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!13 = !{i32 1, !"air.texture", !"air.location_index", i32 7, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>"}
!14 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"uint4"}
!15 = !{i32 3, !"air.vertex_id", !"air.arg_type_name", !"uint"}
!20 = !{ptr @air_fetch_cube_fragment, !7, !21}
!21 = !{!22, !23}
!22 = !{i32 0, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.read", !"air.arg_type_name", !"texturecube<float, read>"}
!23 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"uint4"}
!30 = !{ptr @air_fetch_cube_array_fragment, !7, !31}
!31 = !{!32, !23, !33}
!32 = !{i32 0, !"air.texture", !"air.location_index", i32 6, i32 1, !"air.read", !"air.arg_type_name", !"texturecube_array<float, read>"}
!33 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!40 = !{ptr @air_array_image_transform, !2, !41}
!41 = !{!42, !43, !44}
!42 = !{i32 0, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.read_write", !"air.arg_type_name", !"texture2d_array<float, read_write>"}
!43 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!44 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3"}
!45 = !{ptr @air_cube_image_copy, !2, !46}
!46 = !{!47, !48, !49, !51}
!47 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texturecube<float, read>"}
!48 = !{i32 1, !"air.texture", !"air.location_index", i32 7, i32 1, !"air.write", !"air.arg_type_name", !"texturecube<float, write>"}
!49 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!50 = !{ptr @air_cube_array_sample_vertex, !3, !52}
!51 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3"}
!52 = !{!12, !53, !54, !55, !56, !57}
!53 = !{i32 1, !"air.texture", !"air.location_index", i32 6, i32 1, !"air.sample", !"air.arg_type_name", !"texturecube_array<float, sample>"}
!54 = !{i32 2, !"air.sampler", !"air.location_index", i32 7, i32 1, !"air.arg_type_name", !"sampler"}
!55 = !{i32 3, !"air.buffer", !"air.location_index", i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!56 = !{i32 4, !"air.buffer", !"air.location_index", i32 2, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!57 = !{i32 5, !"air.vertex_id", !"air.arg_type_name", !"uint"}
!60 = !{ptr @air_array_sample_fragment, !7, !61}
!61 = !{!62, !63, !64}
!62 = !{i32 0, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d_array<float, sample>"}
!63 = !{i32 1, !"air.sampler", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"sampler"}
!64 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 8, !"air.arg_type_name", !"uint2"}
!70 = !{ptr @air_cube_sample_fragment, !7, !71}
!71 = !{!72, !63, !73}
!72 = !{i32 0, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.sample", !"air.arg_type_name", !"texturecube<float, sample>"}
!73 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!80 = !{ptr @air_constant_cube_vertex, !3, !52}
!90 = !{ptr @air_constant_array_fragment, !7, !61}
!100 = !{ptr @air_constant_sample, !2, !101}
!101 = !{!102, !103, !104, !105}
!102 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!103 = !{i32 1, !"air.texture", !"air.location_index", i32 5, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d_array<float, sample>"}
!104 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!105 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
!110 = !{ptr @air_mrt_fragment, !111, !115}
!111 = !{!112, !113, !114}
!112 = !{!"air.render_target", i32 7, i32 0, !"air.arg_type_name", !"float4"}
!113 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!114 = !{!"air.render_target", i32 3, i32 0, !"air.arg_type_name", !"float4"}
!115 = !{!116, !117}
!116 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4"}
!117 = !{i32 1, !"air.position", !"air.arg_type_name", !"float4"}
!120 = !{ptr @air_color_only_fragment, !121, !122}
!121 = !{!112}
!122 = !{!116}


; Scalar depth precedes the color member to exercise independent IO allocation.
define {float, <4 x float>} @air_depth_fragment(ptr addrspace(2) %control) {
entry:
  %color = load <4 x float>, ptr addrspace(2) %control, align 16
  %depth = extractelement <4 x float> %color, i32 3
  %a = insertvalue {float, <4 x float>} zeroinitializer, float %depth, 0
  %b = insertvalue {float, <4 x float>} %a, <4 x float> %color, 1
  ret {float, <4 x float>} %b
}
define float @air_depth_only_fragment(<4 x float> %position) {
entry:
  %z = extractelement <4 x float> %position, i32 2
  %near = fmul float %z, 5.000000e-01
  ret float %near
}
declare void @air.discard_fragment()
define <4 x float> @air_discard_fragment(<4 x float> %position, ptr addrspace(1) %output) {
entry:
  store i32 11, ptr addrspace(1) %output, align 4
  %x = extractelement <4 x float> %position, i32 0
  %discard = fcmp olt float %x, 3.200000e+01
  br i1 %discard, label %drop, label %join
drop:
  call void @air.discard_fragment()
  br label %join
join:
  store i32 22, ptr addrspace(1) %output, align 4
  ret <4 x float> %position
}
define void @air_void_fragment() {
entry:
  ret void
}
!130 = !{ptr @air_depth_fragment, !131, !122}
!131 = !{!132, !112}
!132 = !{!"air.depth", !"air.depth_qualifier", !"air.any", !"air.arg_type_name", !"float"}
!135 = !{ptr @air_depth_only_fragment, !136, !138}
!136 = !{!132}
!137 = !{i32 0, !"air.position", !"air.arg_type_name", !"float4"}
!138 = !{!137}
!140 = !{ptr @air_discard_fragment, !7, !141}
!141 = !{!137, !142}
!142 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!150 = !{ptr @air_void_fragment, !2, !2}
