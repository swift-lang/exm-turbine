#!/usr/bin/env bash
# Create C header with arrays containing references to all variable names
# Reads list of file names from stdin

count=0
arrnames=()
while read arrname
do
  arrnames=( "${arrnames[@]}" "${arrname}" )
  # Output variable name for header
  count=$((count + 1))
done

# Print out header file
for arrname in "${arrnames[@]}"
do
  echo "extern const char $arrname[];"
  echo "extern const size_t ${arrname}_len;"
done
echo

echo "static const char *file2array_data[] = {"
for arrname in "${arrnames[@]}"
do
  echo "  $arrname, "
done
echo "};"
echo

echo "static const size_t file2array_data_lens[] = {"
for arrname in "${arrnames[@]}"
do
  echo "  ${arrname}_len, "
done
echo "};"
echo

echo "static const size_t file2array_data_len = $count;"

