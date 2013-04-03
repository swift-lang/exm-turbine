/*
 * Copyright 2013 University of Chicago and Argonne National Laboratory
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

#include <assert.h>
#include <stdio.h>

#include <mpi.h>
#include <tcl.h>

#include <adlb.h>
#include "src/turbine/turbine.h"

int ptasks_1(ClientData cdata, Tcl_Interp *interp,
             int objc, Tcl_Obj *const objv[]);

int
main()
{
  int mpi_argc = 0;
  char** mpi_argv = NULL;

  MPI_Init(&mpi_argc, &mpi_argv);

  // Create communicator for ADLB
  MPI_Comm comm;
  MPI_Comm_dup(MPI_COMM_WORLD, &comm);

  // Build up arguments
  int argc = 0;
  char** argv = NULL;

  Tcl_Interp* interp = Tcl_CreateInterp();
  Tcl_Init(interp);

  Tcl_CreateObjCommand(interp, "ptasks_1_c", ptasks_1,
                       NULL, NULL);

  // Run Turbine
  turbine_code rc =
    turbine_run_interp(comm, "tests/ptasks-bench.tcl", argc, argv, NULL,
                       interp);
  assert(rc == TURBINE_SUCCESS);

  MPI_Finalize();
  return 0;
}

char buffer[1024*1024];

int ptasks_1_impl(MPI_Comm comm, char* arg1);

int
ptasks_1(ClientData cdata, Tcl_Interp *interp,
         int objc, Tcl_Obj *const objv[])
{
  int id;
  int rc;
  rc = Tcl_GetIntFromObj(interp, objv[1], &id);
  assert(rc == TCL_OK);
  adlb_data_type type;
  int length;
  adlb_code code = ADLB_Retrieve(id, &type, 0, buffer, &length);
  assert(code == ADLB_SUCCESS);
  rc = ptasks_1_impl(turbine_task_comm, buffer);
  return rc;
}

int
ptasks_1_impl(MPI_Comm comm, char* arg1)
{
  int size;
  MPI_Comm_size(comm, &size);
  // printf("size: %i\n", size);
  MPI_Barrier(comm);
  // printf("arg1: %s\n", arg1);
  sleep(1);
  return TCL_OK;
}