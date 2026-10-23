target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
@mode = internal addrspace(2) externally_initialized constant i32 undef
@selected = internal addrspace(2) global float 0.0
@llvm.global_ctors = appending global [1 x {i32,ptr,ptr}] [{i32,ptr,ptr} {i32 65535,ptr @initialize,ptr null}]
declare float @air.convert.f.f32.u.i1(i1)
define internal void @initialize() {
entry:
 %m = load i32, ptr addrspace(2) @mode
 switch i32 %m, label %other [i32 0, label %join
                             i32 2, label %shared
                             i32 -1, label %shared]
shared:
 br label %join
other:
 br label %join
join:
 %v = phi i32 [7,%entry],[11,%shared],[19,%other]
 %flag = icmp ne i32 %v, 11
 %numeric = call float @air.convert.f.f32.u.i1(i1 %flag)
 %base = uitofp i32 %v to float
 %result = fadd float %base, %numeric
 store float %result, ptr addrspace(2) @selected
 ret void
}
define void @switch_kernel(ptr addrspace(1) %output) {
 %v = load float, ptr addrspace(2) @selected
 store float %v, ptr addrspace(1) %output
 ret void
}
!air.version = !{!0}
!air.kernel = !{!1}
!air.function_constants = !{!5}
!0 = !{i32 2,i32 7,i32 0}
!1 = !{ptr @switch_kernel,!2,!3}
!2 = !{}
!3 = !{!4}
!4 = !{i32 0,!"air.buffer",!"air.location_index",i32 0,!"air.write",!"air.address_space",i32 1,!"air.arg_type_size",i32 4,!"air.arg_type_name",!"float"}
!5 = !{ptr addrspace(2) @mode,!"uint",!"mode",i32 17,i1 true}
