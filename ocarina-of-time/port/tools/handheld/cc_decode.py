#!/usr/bin/env python3
"""Decode an RDP SetCombineMode (G_SETCOMBINE) command: cc_decode.py FC127E60 FFFFF3F8"""
import sys
A = ["COMBINED","TEXEL0","TEXEL1","PRIMITIVE","SHADE","ENVIRONMENT","1","NOISE"] + ["0"]*8
B = ["COMBINED","TEXEL0","TEXEL1","PRIMITIVE","SHADE","ENVIRONMENT","CENTER","K4"] + ["0"]*8
C = ["COMBINED","TEXEL0","TEXEL1","PRIMITIVE","SHADE","ENVIRONMENT","SCALE","COMBINED_ALPHA","TEXEL0_ALPHA",
     "TEXEL1_ALPHA","PRIMITIVE_ALPHA","SHADE_ALPHA","ENV_ALPHA","LOD_FRACTION","PRIM_LOD_FRAC","K5"] + ["0"]*16
D = ["COMBINED","TEXEL0","TEXEL1","PRIMITIVE","SHADE","ENVIRONMENT","1","0"]
AA = ["COMBINED","TEXEL0","TEXEL1","PRIMITIVE","SHADE","ENVIRONMENT","1","0"]
AC = ["LOD_FRACTION","TEXEL0","TEXEL1","PRIMITIVE","SHADE","ENVIRONMENT","PRIM_LOD_FRAC","0"]
w0, w1 = int(sys.argv[1], 16), int(sys.argv[2], 16)
f = lambda w, s, n: (w >> s) & ((1 << n) - 1)
c1 = (A[f(w0,20,4)], B[f(w1,28,4)], C[f(w0,15,5)], D[f(w1,15,3)])
a1 = (AA[f(w0,12,3)], AA[f(w1,12,3)], AC[f(w0,9,3)], AA[f(w1,9,3)])
c2 = (A[f(w0,5,4)], B[f(w1,24,4)], C[f(w0,0,5)], D[f(w1,6,3)])
a2 = (AA[f(w1,21,3)], AA[f(w1,3,3)], AC[f(w1,18,3)], AA[f(w1,0,3)])
for name, (a, b, c, d) in (("cycle1 rgb", c1), ("cycle1 a  ", a1), ("cycle2 rgb", c2), ("cycle2 a  ", a2)):
    print(f"{name}: ({a} - {b}) * {c} + {d}")
