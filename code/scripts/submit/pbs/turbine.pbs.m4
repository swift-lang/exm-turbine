changecom(`dnl')#!/bin/bash
# We use changecom to change the M4 comment to dnl, not hash

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

# TURBINE.PBS.M4
# Turbine PBS template.  This is automatically filled in
# by M4 in turbine-pbs-run.zsh

# Created: esyscmd(`date')

# Define convenience macros
define(`getenv', `esyscmd(printf -- "$`$1' ")')
define(`getenv_nospace', `esyscmd(printf -- "$`$1'")')

#PBS -N getenv(TURBINE_JOBNAME)
#PBS -l nodes=getenv_nospace(NODES):ppn=getenv(PPN)
#PBS -l walltime=getenv(WALLTIME)
#PBS -j oe
#PBS -o getenv(OUTPUT_FILE)
#PBS -V

set -x

echo "TURBINE-PBS"
date
echo

cd ${PBS_O_WORKDIR}

TURBINE_HOME=getenv(TURBINE_HOME)
PROGRAM=getenv(PROGRAM)

source ${TURBINE_HOME}/scripts/turbine-config.sh
export LD_LIBRARY_PATH=getenv(LD_LIBRARY_PATH)

${TURBINE_LAUNCHER} ${TCLSH} ${PROGRAM}
