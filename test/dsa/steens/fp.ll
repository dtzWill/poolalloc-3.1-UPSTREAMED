; Basic FP resolution
; RUN: dsaopt %s -dsa-steens -analyze -check-same-node=main:val,foo:val
; RUN: dsaopt %s -dsa-steens -analyze -check-same-node=main:val2,main:val
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-gnu"

@FP = external global i8* (i8 *)*

define i64 @main(i32 %argc, i8** %argv) uwtable {
entry:
  %val = alloca i8
  %fptr = load i8*(i8*)** @FP
  store i8*(i8*)* @foo, i8*(i8*)** @FP
  %val2 = call i8* %fptr(i8* %val)
  ret i64 0
}

define i8* @foo(i8* %val) {
entry:
 ret i8* %val
}
