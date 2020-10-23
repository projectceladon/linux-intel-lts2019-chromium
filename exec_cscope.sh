#!/bin/sh

rm tags
rm cscope.*

ctags -R

find $PWD -name "*.h" > cscope.files
find $PWD -name "*.S" >> cscope.files
find $PWD -name "*.c" >> cscope.files
find $PWD -name "*.cpp" >> cscope.files
find $PWD -name "*.mk" >> cscope.files

cscope -bkq -i cscope.files
