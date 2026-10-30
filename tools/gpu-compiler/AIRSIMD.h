// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "Semantic.h"
#include <llvm-c/Core.h>
namespace GPU { namespace Compiler {
inline bool validAIRSIMD(LLVMValueRef call,const AIRSIMD &s,unsigned stage) {
    auto scalar=[](LLVMTypeRef t){return LLVMGetTypeKind(t)==LLVMVectorTypeKind?LLVMGetElementType(t):t;};
    auto lanes=[](LLVMTypeRef t){return LLVMGetTypeKind(t)==LLVMVectorTypeKind?LLVMGetVectorSize(t):1u;};
    auto integer=[](LLVMTypeRef t,unsigned bits){return LLVMGetTypeKind(t)==LLVMIntegerTypeKind && LLVMGetIntTypeWidth(t)==bits;};
    auto t=LLVMTypeOf(call),leaf=scalar(t);
    if(stage || LLVMGetNumArgOperands(call)!=s.count || lanes(t)!=s.lanes)return false;
    if(s.floating ? (s.bits==16?LLVMGetTypeKind(leaf)!=LLVMHalfTypeKind:LLVMGetTypeKind(leaf)!=LLVMFloatTypeKind) : !integer(leaf,s.bits))return false;
    if(!s.count)return true;
    if(LLVMTypeOf(LLVMGetOperand(call,0))!=(s.op=="ballot"?LLVMInt1TypeInContext(LLVMGetTypeContext(t)):t))return false;
    if(s.count==2 && !integer(LLVMTypeOf(LLVMGetOperand(call,1)),16))return false;
    if(s.count==4 && (LLVMTypeOf(LLVMGetOperand(call,1))!=t || !integer(LLVMTypeOf(LLVMGetOperand(call,2)),16) ||
        !integer(LLVMTypeOf(LLVMGetOperand(call,3)),16)))return false;
    if(s.floating && (s.op=="and" || s.op=="or" || s.op=="xor"))return false;
    return true;
}
} }
