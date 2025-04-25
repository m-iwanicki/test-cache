#!/bin/bash

ARGS_1=(0)
ARGS_2=(250)
TIME_1=()
TIME_2=()

for i in {1..4}; do
    echo "Iteration $i"
    TIME_1+=( "$(LD_LIBRARY_PATH=bin bin/test_libflush "${ARGS_1[@]}" 2>/dev/null)" )
    TIME_2+=( "$(LD_LIBRARY_PATH=bin bin/test_libflush "${ARGS_2[@]}" 2>/dev/null)" )
done

echo "time_1: ${TIME_1[*]}"
echo "time_2: ${TIME_2[*]}"

echo -n "Run 1 average time: "
{ 
    echo -n "scale=1; (";
    echo -n "${TIME_1[*]}" | tr " " "+";
    echo ")/${#TIME_1[@]}";
} | bc -l
MEDIAN=($(echo "${TIME_1[*]}" | tr " " "\n" | sort -n))
echo  "Run 1 median time: ${MEDIAN[$((${#MEDIAN[@]}/2))]}"

echo -n "Run 2 average time: "
{ 
    echo -n "scale=1; (";
    echo -n "${TIME_2[*]}" | tr " " "+";
    echo ")/${#TIME_2[@]}";
} | bc -l
MEDIAN=($(echo "${TIME_2[*]}" | tr " " "\n" | sort -n))
echo  "Run 2 median time: ${MEDIAN[$((${#MEDIAN[@]}/2))]}"
