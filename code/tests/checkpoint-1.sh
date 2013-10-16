#!/bin/bash
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

TESTS=$( dirname $0 )

set -x

THIS=$0
BIN=${THIS%.sh}.x
OUTPUT=${THIS%.sh}.out

export PROCS=4
${TESTS}/run-mpi.zsh ${BIN} >& ${OUTPUT}
[[ ${?} == 0 ]] || exit 1

grep WARNING ${OUTPUT} && exit 1

F1=./checkpoint-1.xpt
if [[ ! -f $F1 ]]; then
  echo "$F1 not created"
  exit 1
fi
F1_BYTES=$(wc -c $F1)
echo "$F1 $F1_BYTES bytes"
# Sanity check file length
if [[ $F1_BYTES -lt 1024 ]]; then
  echo "$F1 size wayyyy too small"
  exit 1
fi

# Cleanup on success
rm $F1

exit 0
