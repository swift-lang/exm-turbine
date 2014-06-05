#!/bin/zsh -ef

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

# TURBINE-APRUN-RUN
# Creates a APRUN run file and runs it on the given program

# usage:
#  turbine-aprun-run.zsh -n <PROCS> [-e <ENV>]* [-o <OUTPUT>] -t <WALLTIME>
#                        [-x] [-X]
#                           <SCRIPT> [<ARG>]*
#
# -x: if provide, program is executable rather than Tcl script
# -X: use turbine_sh launcher instead of tclsh

# Environment variables that may be set:
# QUEUE: The queue name to use
# PROJECT: The project name to use (default none)
# PPN:            Processes-per-node
# WALLTIME:       Time limit.  Default: 00:15:00 (15 minutes)
# TURBINE_OUTPUT_ROOT: Where to put Turbine output-
#          a subdirectory based on the current time
#          will be created, reported, and used
#          (default ~/turbine-output)
# TURBINE_OUTPUT: Directory in which to place output
# MPICH_CUSTOM_RANK_ORDER: executable that prints Mpich rank order file
#          to standard output, for MPICH_RANK_REORDER_METHOD=3

# Runs job in TURBINE_OUTPUT
# Pipes output and error to TURBINE_OUTPUT/output.txt
# Creates TURBINE_OUTPUT/log.txt and TURBINE_OUTPUT/jobid.txt

# Convention note: This script uses -n <processes>
# (We follow the mpiexec convention.)

# NOTE: See the sourced helpers.zsh script for definitions of some
#       shell functions used here.

export TURBINE_HOME=$( cd $( dirname $0 )/../../.. ; /bin/pwd )
if [[ ${?} != 0 ]]
then
  print "Could not find Turbine installation!"
  return 1
fi
# declare TURBINE_HOME

source ${TURBINE_HOME}/scripts/submit/run-init.zsh

# Set SCRIPT_NAME, make PROGRAM an absolute path
export SCRIPT_NAME=$( basename ${SCRIPT} )
pushd $( dirname ${SCRIPT} ) >& /dev/null
SCRIPT_DIR=$( /bin/pwd )
popd >& /dev/null
SCRIPT=${SCRIPT_DIR}/${SCRIPT_NAME}

checkvars SCRIPT PPN TURBINE_OUTPUT WALLTIME
declare   SCRIPT PPN TURBINE_OUTPUT WALLTIME QUEUE

# Round NODES up for extra processes
export NODES=$(( PROCS/PPN ))
(( PROCS % PPN )) && (( NODES++ )) || true
declare PROCS NODES

# Setup custom rank order
if (( ${+MPICH_CUSTOM_RANK_ORDER} ))
then
  if [[ ! -x "${MPICH_CUSTOM_RANK_ORDER}" ]]
  then
    print "Expected MPICH_CUSTOM_RANK_ORDER=${MPICH_CUSTOM_RANK_ORDER} to \
           be an executable file.  Aborting."
    exit 1
  fi

  ${MPICH_CUSTOM_RANK_ORDER} ${NODES} > ${TURBINE_OUTPUT}/MPICH_RANK_ORDER
  export MPICH_RANK_REORDER_METHOD=3
fi

# Filter the template to create the PBS submit script
TURBINE_APRUN_M4=${TURBINE_HOME}/scripts/submit/cray/turbine-aprun.sh.m4
TURBINE_APRUN=${TURBINE_OUTPUT}/turbine-aprun.sh

mkdir -pv ${TURBINE_OUTPUT}
touch ${TURBINE_APRUN}

m4 ${TURBINE_APRUN_M4} > ${TURBINE_APRUN}

print "wrote: ${TURBINE_APRUN}"

QUEUE_ARG=""
[[ ${QUEUE} != "" ]] && QUEUE_ARG="-q ${QUEUE}"

for kv in ${env}
do
  print "user environment variable: ${kv}"
  export ${kv}
done

(( ! ${+QSUB_OPTS} )) && QSUB_OPTS=""

qsub ${=QUEUE_ARG} ${=QSUB_OPTS} ${TURBINE_OUTPUT}/turbine-aprun.sh | \
  read JOB_ID
EXITCODE=${?}
# Return exit code from qsub

if (( EXITCODE ))
then
  print "qsub failed!"
  exit ${EXITCODE}
fi

declare JOB_ID
print ${JOB_ID} > ${TURBINE_OUTPUT}/jobid.txt

LOG_FILE=${TURBINE_OUTPUT}/turbine.log
{
  print "JOB:               ${JOB_ID}"
  print "COMMAND:           ${SCRIPT_NAME} ${ARGS}"
  print "HOSTNAME:          $( hostname -d )"
  print "SUBMITTED:         $( date_nice )"
  print "PROCS:             ${PROCS}"
  print "PPN:               ${PPN}"
  print "NODES:             ${NODES}"
  print "TURBINE_WORKERS:   ${TURBINE_WORKERS}"
  print "ADLB_SERVERS:      ${ADLB_SERVERS}"
  print "WALLTIME:          ${WALLTIME}"
  print "ADLB_EXHAUST_TIME: ${ADLB_EXHAUST_TIME}"
} >> ${LOG_FILE}
