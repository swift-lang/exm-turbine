#!/usr/bin/env bash

# Copyright 2013 University of Chicago and Argonne National Laboratory
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License

# Create C header with an array containing references to all variable names
# Usage: files2arrays_mkhdr <C array name> <file2array files>

set -e

master_arr=""
modifiers=""

usage () {
  echo "Usage: $0 [ -v <c master array variable name> ]\
    [ -m <extern array variable modifiers> ]\
     <input file>* " >&2
  exit 1
}

while getopts "v:m:" opt; do
  case $opt in 
    v) 
      if [[ $master_arr != "" ]]; then
        echo "-v specified twice" >&2
        usage
      fi
      master_arr=$OPTARG
      ;;
    m)
      if [[ $modifiers != "" ]]; then
        echo "-m specified twice" >&2
        usage
      fi
      modifiers=$OPTARG
      ;;
    \?)
      echo "Invalid option: -$OPTARG" >&2
      usage
  esac
done

shift $((OPTIND - 1))

if [ -z "$modifiers" ]; then
  # Default is const with global linking visibility
  modifiers="const"
fi

count=0
arrnames=()
filenames=()
for file in "$@"
do
  # Extract array name from FILE2ARRAY comment
  arrname=$(sed -rn 's/^.*\/\*FILE2ARRAY:([a-zA-Z_0-9]*):(.*)\*\/.*$/\1/p' $file)
  if [ -z "$arrname" ]; then
    echo "Could not extract array name from $file" >&2
    exit 1
  fi
  arrnames=( "${arrnames[@]}" "${arrname}" )
  filename=$(sed -rn 's/^.*\/\*FILE2ARRAY:([a-zA-Z_0-9]*):(.*)\*\/.*$/\2/p' $file)
  if [ -z "$filename" ]; then
    echo "Could not extract file name from $file"  >&2
    exit 1
  fi
  filenames=( "${filenames[@]}" "${filename}" )
  # Output variable name for header
  count=$((count + 1))
done

echo "/* AUTOGENERATED HEADER - DO NOT EDIT */"
echo

echo "#include <stddef.h>" # For size_t
echo

# Print out header file
for arrname in "${arrnames[@]}"
do
  echo "extern $modifiers unsigned char $arrname[];"
  echo "extern $modifiers size_t ${arrname}_len;"
done
echo

# Arrays indexing above variables
len_arr=${master_arr}_lens
name_arr=${master_arr}_names
echo "static const char *${master_arr}[${count}];"
echo "static size_t ${len_arr}[${count}];"
echo "static const char *${name_arr}[${count}];"
echo "static const size_t ${master_arr}_len = $count;"
echo

# List of filenames
echo "static const char *${master_arr}_filenames[] = {"
for filename in "${filenames[@]}"
do
  echo "  \"$filename\","
done
echo "};"
echo

# Generate initializer function for arrays
echo "static inline void ${master_arr}_init(void) {"

i=0
for arrname in "${arrnames[@]}"
do
  echo "  ${master_arr}[${i}] = $arrname;"
  echo "  ${len_arr}[${i}] = ${arrname}_len;"
  echo "  ${name_arr}[${i}] = \"${arrname}\";"
  i=$(($i + 1))
done
echo "}"
echo
