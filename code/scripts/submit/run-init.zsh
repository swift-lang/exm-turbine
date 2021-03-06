
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

# RUN-INIT

# Common queue  submission setup file used by Cobalt and PBS
#   (and APRUN)
# Used to process command line arguments, initialize basic settings
# before launching qsub

set -e

source ${TURBINE_HOME}/scripts/turbine-config.sh
source ${TURBINE_HOME}/scripts/helpers.zsh

turbine_log()
# Fills in turbine.log file after job completion
{
  print "JOB:               ${JOB_ID}"
  print "COMMAND:           ${SCRIPT_NAME} ${ARGS}"
  print "WORK_DIRECTORY:    ${WORK_DIRECTORY}"
  print "HOSTNAME:          $( hostname -d )"
  print "SUBMITTED:         $( date_nice )"
  print "PROCS:             ${PROCS}"
  print "PPN:               ${PPN}"
  print "TURBINE_WORKERS:   ${TURBINE_WORKERS}"
  print "ADLB_SERVERS:      ${ADLB_SERVERS}"
  print "WALLTIME:          ${WALLTIME}"
  print "ADLB_EXHAUST_TIME: ${ADLB_EXHAUST_TIME}"
}

# Defaults:
CHANGE_DIRECTORY=""
export EXEC_SCRIPT=0 # 1 means execute script directly, e.g. if binary
export TURBINE_STATIC_EXEC=0 # Use turbine_sh instead of tclsh
INIT_SCRIPT=0
declare PROCS
(( ! ${+PROCS} )) && PROCS=0
[[ ${PROCS} == "" ]] && PROCS=0
export PROCS
if (( ! ${+TURBINE_OUTPUT_ROOT} ))
then
  TURBINE_OUTPUT_ROOT=${HOME}/turbine-output
fi
SETTINGS=0
export WALLTIME=${WALLTIME:-00:15:00}
export VERBOSE=0
export PPN=${PPN:-1}

# Place to store output directory name
OUTPUT_TOKEN_FILE=turbine-directory.txt

# Job environment
typeset -T ENV env
env=()

# Get options
while getopts "C:d:e:i:n:o:s:t:VxX" OPTION
 do
  case ${OPTION}
   in
    C)
      CHANGE_DIRECTORY=${OPTARG}
      ;;
    d)
      OUTPUT_TOKEN_FILE=${OPTARG}
      ;;
    e)
      env+=${OPTARG}
      ;;
    i)
       INIT_SCRIPT=${OPTARG}
       ;;
    n) PROCS=${OPTARG}
      ;;
    o) TURBINE_OUTPUT_ROOT=${OPTARG}
      ;;
    s) SETTINGS=${OPTARG}
      ;;
    t) WALLTIME=${OPTARG}
      ;;
    V)
      VERBOSE=1
      ;;
    x)
      export EXEC_SCRIPT=1
      ;;
    X)
      export TURBINE_STATIC_EXEC=1
      ;;
    *)
      print "abort"
      exit 1
      ;;
  esac
done
shift $(( OPTIND-1 ))

if (( VERBOSE ))
then
  set -x
fi

if [[ ${PROCS} != <-> ]]
then
  print "PROCS must be an integer: you set PROCS=${PROCS}"
  return 1
fi

export SCRIPT=$1
checkvar SCRIPT
shift
export ARGS="${*}"

if [[ ${SETTINGS} != 0 ]]
then
  print "sourcing: ${SETTINGS}"
  if ! source ${SETTINGS}
  then
    print "script failed: ${SETTINGS}"
    return 1
  fi
  print "done: ${SETTINGS}"
fi

[[ -f ${SCRIPT} ]] || abort "Could not find script: ${SCRIPT}"

START=$( date +%s )

if [[ ${PROCS} == 0 ]]
  then
  print "The process count was not specified!"
  print "Use the -n argument or set environment variable PROCS."
  exit 1
fi

# Prints a TURBINE_OUTPUT directory
# Handles TURBINE_OUTPUT_FORMAT
# Default format is e.g., 2006/10/13/14/26/12
turbine_output_format()
{
  TURBINE_OUTPUT_FORMAT=${TURBINE_OUTPUT_FORMAT:-%Y/%m/%d/%H/%M/%S}
  local S=$( date +${TURBINE_OUTPUT_FORMAT} )
  if [[ ${S} == *%Q* ]]
  then
    # Create a unique directory by substituting on %Q
    TURBINE_OUTPUT_PAD=${TURBINE_OUTPUT_PAD:-3}
    integer -Z ${TURBINE_OUTPUT_PAD} i=1
    while true
    do
      local D=${S/\%Q/${i}}
      local TRY=${TURBINE_OUTPUT_ROOT}/${D}
      [[ ! -d ${TRY} ]] && break
      (( i++ ))
    done
    print ${TRY}
  else
    print ${TURBINE_OUTPUT_ROOT}/${S}
  fi
}

# Create the directory in which to run
if (( ! ${+TURBINE_OUTPUT} ))
then
  export TURBINE_OUTPUT=$( turbine_output_format )
fi
export TURBINE_OUTPUT
declare TURBINE_OUTPUT

# All output from job, including error stream
export OUTPUT_FILE=${TURBINE_OUTPUT}/output.txt

print ${TURBINE_OUTPUT} > ${OUTPUT_TOKEN_FILE}
mkdir -p ${TURBINE_OUTPUT}

if [[ ${INIT_SCRIPT} != 0 ]]
then
  print "executing: ${INIT_SCRIPT}"
  if ! ${INIT_SCRIPT}
  then
    print "script failed: ${INIT_SCRIPT}"
    return 1
  fi
  print "done: ${INIT_SCRIPT}"
fi

# Log file for turbine-cobalt settings
LOG_FILE=${TURBINE_OUTPUT}/turbine.log
# All output from job, including error stream
OUTPUT_FILE=${TURBINE_OUTPUT}/output.txt

print "SCRIPT:            ${SCRIPT}" >> ${LOG_FILE}
SCRIPT_NAME=$( basename ${SCRIPT} )
cp -v ${SCRIPT} ${TURBINE_OUTPUT}

JOB_ID_FILE=${TURBINE_OUTPUT}/jobid.txt

## Local Variables:
## mode: sh
## End:
