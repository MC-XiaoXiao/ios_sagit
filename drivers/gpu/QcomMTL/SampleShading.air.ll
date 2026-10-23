; SPDX-License-Identifier: BSD-2-Clause
; Authored AIR examples using the public sample input/output metadata recorded
; in Source.json. Neither body is captured from an Apple binary.
source_filename = "SampleShading.air.ll"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v96:128:128-v128:128:128-n8:16:32"
target triple = "air64_v27-apple-ios18.5.0"

%SampleResult = type { <4 x float>, i32 }

define <4 x float> @air_sample_color(i32 %sample) {
    %s = uitofp i32 %sample to float
    %r = fmul float %s, 0.25
    %c = insertelement <4 x float> <float 0.0, float 0.5, float 1.0, float 1.0>, float %r, i32 0
    ret <4 x float> %c
}

define %SampleResult @air_sample_mask(i32 %sample, i32 %coverage) {
    %bit = shl i32 1, %sample
    %mask = and i32 %coverage, %bit
    %color = insertvalue %SampleResult poison, <4 x float> <float 1.0, float 0.5, float 0.25, float 1.0>, 0
    %out = insertvalue %SampleResult %color, i32 %mask, 1
    ret %SampleResult %out
}

!air.fragment = !{!0, !1}
!air.version = !{!10}
!air.language_version = !{!11}
!0 = !{ptr @air_sample_color, !2, !3}
!1 = !{ptr @air_sample_mask, !4, !5}
!2 = !{!6}
!3 = !{!7}
!4 = !{!6, !8}
!5 = !{!7, !9}
!6 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!7 = !{i32 0, !"air.sample_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"sample"}
!8 = !{!"air.sample_mask", !"air.arg_type_name", !"uint", !"air.arg_name", !"coverage"}
!9 = !{i32 1, !"air.sample_mask", !"air.arg_type_name", !"uint", !"air.arg_name", !"coverage"}
!10 = !{i32 2, i32 7, i32 0}
!11 = !{!"Metal", i32 3, i32 2, i32 0}
