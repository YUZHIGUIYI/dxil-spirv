#version 460

layout(location = 0) in vec4 A;
layout(location = 0) out float SV_Target;

void main()
{
    float _28;
    if (isnan(A.x))
    {
        float frontier_phi_3_1_ladder;
        if (isnan(A.y))
        {
            frontier_phi_3_1_ladder = A.z;
        }
        else
        {
            frontier_phi_3_1_ladder = A.w;
        }
        _28 = frontier_phi_3_1_ladder;
    }
    else
    {
        _28 = A.w;
    }
    SV_Target = _28;
}


#if 0
// SPIR-V disassembly
; SPIR-V
; Version: 1.3
; Generator: Unknown(30017); 21022
; Bound: 37
; Schema: 0
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %3 "main" %8 %10
OpExecutionMode %3 OriginUpperLeft
OpName %3 "main"
OpName %8 "A"
OpName %10 "SV_Target"
OpName %29 "frontier_phi_3.1.ladder"
OpDecorate %8 Location 0
OpDecorate %10 Location 0
%1 = OpTypeVoid
%2 = OpTypeFunction %1
%5 = OpTypeFloat 32
%6 = OpTypeVector %5 4
%7 = OpTypePointer Input %6
%8 = OpVariable %7 Input
%9 = OpTypePointer Output %5
%10 = OpVariable %9 Output
%11 = OpTypePointer Input %5
%13 = OpTypeInt 32 0
%14 = OpConstant %13 0
%17 = OpConstant %13 3
%19 = OpTypeBool
%22 = OpConstant %13 2
%25 = OpConstant %13 1
%3 = OpFunction %1 None %2
%4 = OpLabel
OpBranch %30
%30 = OpLabel
%12 = OpAccessChain %11 %8 %14
%15 = OpLoad %5 %12
%16 = OpAccessChain %11 %8 %17
%18 = OpLoad %5 %16
%20 = OpIsNan %19 %15
OpSelectionMerge %35 None
OpBranchConditional %20 %32 %31
%32 = OpLabel
%21 = OpAccessChain %11 %8 %22
%23 = OpLoad %5 %21
%24 = OpAccessChain %11 %8 %25
%26 = OpLoad %5 %24
%27 = OpIsNan %19 %26
OpSelectionMerge %34 None
OpBranchConditional %27 %34 %33
%33 = OpLabel
OpBranch %34
%34 = OpLabel
%29 = OpPhi %5 %23 %32 %18 %33
OpBranch %35
%31 = OpLabel
OpBranch %35
%35 = OpLabel
%28 = OpPhi %5 %18 %31 %29 %34
OpStore %10 %28
OpReturn
OpFunctionEnd
#endif
