;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(64,1,1)
; MinimumExpectedWaveLaneCount: 0
; MaximumExpectedWaveLaneCount: 4294967295
; UsesViewID: false
; SigInputElements: 0
; SigOutputElements: 0
; SigPatchConstOrPrimElements: 0
; SigInputVectors: 0
; SigOutputVectors[0]: 0
; SigOutputVectors[1]: 0
; SigOutputVectors[2]: 0
; SigOutputVectors[3]: 0
; EntryFunctionName: negated
;
;
; Buffer Definitions:
;
; cbuffer 
; {
;
;   [16 x i8] (type annotation not present)
;
; }
;
; Resource bind info for 
; {
;
;   [4 x i8] (type annotation not present)
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
;                                   cbuffer      NA          NA     CB0    cb2,space12     1
;                                       UAV  struct         r/w      U0             u0     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:8-i16:16-i32:32-i64:64-f16:16-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResBind = type { i32, i32, i32, i8 }
%dx.types.ResourceProperties = type { i32, i32 }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%sgl.resource.1.0 = type { i32 }
%sgl.resource.2.0 = type { i32 }

define void @negated() {
  %1 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 2, i32 2, i32 12, i8 2 }, i32 2, i1 false)  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %1, %dx.types.ResourceProperties { i32 13, i32 16 })  ; AnnotateHandle(res,props)  resource: CBuffer
  %3 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 0, i32 0, i32 0, i8 1 }, i32 0, i1 false)  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %4 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %3, %dx.types.ResourceProperties { i32 4108, i32 4 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=4>
  %5 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  %6 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)
  %7 = call i32 @dx.op.threadId.i32(i32 93, i32 2)  ; ThreadId(component)
  %8 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %2, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %9 = extractvalue %dx.types.CBufRet.i32 %8, 0
  %10 = icmp ult i32 %5, %9
  %11 = xor i1 %10, true
  br i1 %11, label %16, label %12

; <label>:12                                      ; preds = %0
  %13 = extractvalue %dx.types.CBufRet.i32 %8, 1
  %14 = icmp ult i32 %6, %13
  %15 = xor i1 %14, true
  br label %16

; <label>:16                                      ; preds = %12, %0
  %17 = phi i1 [ %11, %0 ], [ %15, %12 ]
  br i1 %17, label %22, label %18

; <label>:18                                      ; preds = %16
  %19 = extractvalue %dx.types.CBufRet.i32 %8, 2
  %20 = icmp ult i32 %7, %19
  %21 = xor i1 %20, true
  br label %22

; <label>:22                                      ; preds = %18, %16
  %23 = phi i1 [ %17, %16 ], [ %21, %18 ]
  br i1 %23, label %24, label %25

; <label>:24                                      ; preds = %22
  ret void

; <label>:25                                      ; preds = %22
  %26 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %4, i32 %5, i32 0, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %27 = extractvalue %dx.types.ResRet.i32 %26, 0
  %28 = icmp ne i32 %27, 3
  %29 = xor i1 %28, true
  br i1 %29, label %30, label %31

; <label>:30                                      ; preds = %25
  ret void

; <label>:31                                      ; preds = %25
  %32 = shl i32 %27, 1
  %33 = mul i32 %5, 100
  %34 = add i32 %32, %33
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %4, i32 %5, i32 0, i32 %34, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  ret void
}

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #0

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #1

; Function Attrs: nounwind
declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32) #2

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind readonly }
attributes #2 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!10}

!0 = !{!"sgl"}
!1 = !{i32 1, i32 6}
!2 = !{i32 1, i32 9}
!3 = !{!"cs", i32 6, i32 6}
!4 = !{null, !5, !8, null}
!5 = !{!6}
!6 = !{i32 0, %sgl.resource.1.0* undef, !"", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !7}
!7 = !{i32 1, i32 4}
!8 = !{!9}
!9 = !{i32 0, %sgl.resource.2.0* undef, !"", i32 12, i32 2, i32 1, i32 16, null}
!10 = !{void ()* @negated, !"negated", null, !4, !11}
!11 = !{i32 0, i64 8388624, i32 4, !12}
!12 = !{i32 64, i32 1, i32 1}

