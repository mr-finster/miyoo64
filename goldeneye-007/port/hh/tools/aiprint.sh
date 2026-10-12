#!/bin/sh
# The decomp's ConvertAIPRINT (Makefile:53): turn PRINT("text") in the AI lists into
# AI_PRINT,'t','e','x','t','\0', exactly as the N64 build pipes chraidata.c before IDO.
exec sed -E -e ':loop s/PRINT\("(..*?)(.)"/PRINT\("\1",\x27\2\x27/g; tloop;
                s/(PRINT\(.*?)\x27\\\x27,\x27(.)\x27(.*)\)/\1\x27\\\2\x27\3\)/g;
                s/(PRINT\()"(.)"(.*)\)/\1\x27\2\x27\3\)/g;
                s/PRINT\((.*)\)/PRINT\(\1,\x27\\0\x27\,)/g;
                s/PRINT\((.*)\)/AI_PRINT,\1/g' "$1"
