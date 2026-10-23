// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <cstring>
#include "AIRSpecialization.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/IRReader.h>
// Exercise body deletion with PHIs/backedges and cascading dead calls while
// retaining AIR metadata, selected entries and address-taken/recursive bodies.
inline bool checkAIRDeadHelpers() {
    const char *ir=R"ir(
target datalayout = "e-p:64:64"
@escaped = global ptr @address_taken
define i32 @leaf(i32 %x) { ret i32 %x }
define i32 @orphan(i1 %cond) {
entry:
  br label %loop
loop:
  %n = phi i32 [ 0, %entry ], [ %next, %loop ]
  %next = call i32 @leaf(i32 %n)
  br i1 %cond, label %loop, label %exit
exit:
  ret i32 %next
}
define i32 @address_taken(i32 %x) { ret i32 %x }
define i32 @recursive(i32 %x) {
  %v = call i32 @recursive(i32 %x)
  ret i32 %v
}
define void @selected() { ret void }
define void @alternate() { ret void }
!air.kernel = !{!0, !1}
!0 = !{ptr @selected, !2, !2}
!1 = !{ptr @alternate, !2, !2}
!2 = !{}
)ir";
    for(const std::string selected:{"selected","alternate",""}) {
        auto context=LLVMContextCreate();LLVMModuleRef module=nullptr;char *error=nullptr;
        auto buffer=LLVMCreateMemoryBufferWithMemoryRangeCopy(ir,std::strlen(ir),"dead helpers");
        if(LLVMParseIRInContext(context,buffer,&module,&error)) {
            LLVMDisposeMessage(error);LLVMContextDispose(context);return false;
        }
        bool ok=true;
        {
            GPU::Compiler::AIRSpecialization::State state(module,selected);state.apply({});state.lowerHelpers();
            ok=!LLVMVerifyModule(module,LLVMReturnStatusAction,&error);
            LLVMDisposeMessage(error);
            for(const char *name:{"leaf","orphan"})ok=ok && LLVMCountBasicBlocks(LLVMGetNamedFunction(module,name))==0;
            for(const char *name:{"address_taken","recursive"})ok=ok && LLVMCountBasicBlocks(LLVMGetNamedFunction(module,name))!=0;
            for(const char *name:{"selected","alternate"})
                ok=ok && bool(LLVMCountBasicBlocks(LLVMGetNamedFunction(module,name)))==(selected.empty() || selected==name);
            ok=ok && LLVMGetNamedMetadataNumOperands(module,"air.kernel")==2;
        }
        LLVMDisposeModule(module);LLVMContextDispose(context);if(!ok)return false;
    }
    return true;
}
