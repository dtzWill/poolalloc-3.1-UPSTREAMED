; Basic FP resolution
; RUN: dsaopt %s -dsa-steens -analyze -check-same-node=main:mval,foo:fval
; RUN: dsaopt %s -dsa-steens -analyze -check-same-node=main:mval2,main:mval
; RUN: dsaopt %s -dsa-steens -analyze -check-same-node=call:cval,main:mval
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-gnu"

define internal i8* @call(i8* (i8*)* %fp, i8* %arg) {
entry:
  %cval = call i8* %fp(i8* %arg)
  ret i8* %cval
}

define i64 @main(i32 %argc, i8** %argv) uwtable {
entry:
  %mval = alloca i8
  %mval2 = call i8* @call(i8*(i8*)* @foo, i8* %mval)
  ret i64 0
}

define internal i8* @foo(i8* %fval) {
entry:
 ret i8* %fval
}

