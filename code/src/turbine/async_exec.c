/*
 * Copyright 2014 University of Chicago and Argonne National Laboratory
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

/*
  Generic async executor interface

  Created by Tim Armstrong, Nov 2013

Architecture
------------
* Servers + regular workers + async work managers.
* Each async work manager has multiple async tasks executing
  simultaneously
* We want to be able to execute arbitrary code before and after each
  task on the worker node (to fetch data, store data, etc).
  - When starting a task, the async work manager must execute
    compiler-generated code.  This code is responsible for launching
    the async task.  I.e. async worker gets task, async worker executes
    task, hands control to task code, task code calls function to
    launch async task, thereby returning control to async work manager
    temporarily.
  - Each async work task has two callbacks associated that are called
    back in case of success/failure.


Assumptions
-----------
* 1 ADLB worker per work-type, with N slots
* Compiler generates code with 1 work unit per task, plus optionally a
  chain of callbacks that may also contain 1 work unit of that type each
* Add ability to request multiple tasks


Implications of Assumptions
---------------------------
* 1 ADLB Get per slot to fill
* ADLB get only needs to get one work type
* Can do blocking ADLB get if no work
* Have to check slots after executing each task or callback:
  async worker code doesn't know if task code added work.

 */
#define _GNU_SOURCE // for asprintf()
#include <stdio.h>

#include "src/turbine/turbine-checks.h"
#include "src/turbine/async_exec.h"
#include "src/turbine/executors/exec_interface.h"
#include "src/turbine/services.h"

#include <assert.h>
#include <sched.h>

#include <adlb.h>
#include <table.h>

#define COMPLETED_BUFFER_SIZE 16


/* Initialization of module */
static bool executors_init = false;

/* Lazy initialization of executors table - may register executor at
   any time  */
static bool executors_table_init = false;
static struct table executors;

static turbine_code init_exec_table(void);

static turbine_exec_code
get_tasks(Tcl_Interp *interp, turbine_executor *executor,
          int adlb_work_type, void *buffer, size_t buffer_size,
          bool poll, int max_tasks, bool *got_tasks);

static turbine_exec_code
check_tasks(Tcl_Interp *interp, turbine_executor *executor, bool poll,
            bool *task_completed);

static void
stop_executors(turbine_executor *executors, int nexecutors);

static void
launch_error(Tcl_Interp* interp, turbine_executor *exec, int tcl_rc,
             const char *command);

static void
callback_error(Tcl_Interp* interp, turbine_executor *exec, int tcl_rc,
               Tcl_Obj *command);

turbine_code
turbine_async_exec_initialize(void)
{
  turbine_condition(!executors_init, TURBINE_ERROR_INVALID,
                    "Executors already init");

  executors_init = true;
  return TURBINE_EXEC_SUCCESS;
}

static turbine_code
init_exec_table(void)
{
  assert(!executors_table_init);
  bool ok = table_init(&executors, 16);
  turbine_condition(ok, TURBINE_ERROR_OOM, "Error initializing table");
  
  executors_table_init = true;
  return TURBINE_SUCCESS;
}


turbine_code
turbine_add_async_exec(turbine_executor executor)
{
  if (!executors_table_init)
  {
    turbine_code tc = init_exec_table();
    turbine_check(tc);
  }

  turbine_condition(executors.size < TURBINE_ASYNC_EXECUTOR_LIMIT,
        TURBINE_ERROR_INVALID,
        "Adding %s would exceed limit of %i async executors",
        executor.name, TURBINE_ASYNC_EXECUTOR_LIMIT);

  // TODO: ownership of pointers, etc
  // TODO: validate executor
  turbine_executor *exec_ptr = malloc(sizeof(executor));
  TURBINE_MALLOC_CHECK(exec_ptr);
  *exec_ptr = executor;

  table_add(&executors, executor.name, exec_ptr);

  return TURBINE_SUCCESS;
}

turbine_code
turbine_async_exec_names(const char **names, int size, int *count)
{
  int n = 0;

  if (!executors_table_init) {
    *count = 0;
    return TURBINE_SUCCESS;
  }

  TABLE_FOREACH(&executors, entry)
  {
    if (n == size) {
      break;
    }

    names[n++] = entry->key;
  }

  *count = n;
  return TURBINE_SUCCESS;
}

turbine_executor *
turbine_get_async_exec(const char *name)
{
  if (!executors_table_init)
  {
    return NULL;
  }

  turbine_executor *executor;
  if (!table_search(&executors, name, (void**)&executor)) {
    printf("Could not find executor: \"%s\"\n", name);
    return NULL;
  }
  return executor;
}

turbine_code
turbine_configure_exec(turbine_executor *exec, const char *config,
                       size_t config_len)
{
  assert(exec != NULL);
  turbine_exec_code ec;
  
  if (exec->configure != NULL)
  {
    ec = exec->configure(&exec->context, config, config_len);
    TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
                 "error configuring executor %s", exec->name);
  }

  return TURBINE_SUCCESS;
}

turbine_code
turbine_async_worker_loop(Tcl_Interp *interp, turbine_executor *exec,
                int adlb_work_type, void *buffer, size_t buffer_size)
{
  turbine_exec_code ec;
  turbine_code tc;

  tc = turbine_service_init();
  turbine_check(tc);

  assert(exec != NULL);
  assert(buffer != NULL);
  assert(buffer_size > 0);
  // TODO: check buffer large enough for work units

  assert(exec->start != NULL);
  ec = exec->start(exec->context, &exec->state);
  TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error starting executor %s", exec->name);

  while (true)
  {
    turbine_exec_slot_state slots;
    bool something_happened = false;
    ec = exec->slots(exec->state, &slots);
    TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error getting executor slot count %s", exec->name);

    if (slots.used < slots.total)
    {
      int max_tasks = slots.total - slots.used;

      // Need to do non-blocking get if we're polling executor too
      bool poll = (slots.used != 0);
      
      ec = get_tasks(interp, exec, adlb_work_type, buffer, buffer_size,
                     poll, max_tasks, &something_happened);
      if (ec == TURBINE_EXEC_SHUTDOWN)
      {
        break;
      }
      TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error getting tasks for executor %s", exec->name);
    }
    // Update count in case work added 
    ec = exec->slots(exec->state, &slots);
    TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error getting executor slot count %s", exec->name);

    if (slots.used > 0)
    {
      // Need to do non-blocking check if we want to request more work
      bool poll = (slots.used < slots.total);
      ec = check_tasks(interp, exec, poll, &something_happened);
      TURBINE_EXEC_CHECK(ec, TURBINE_ERROR_EXTERNAL);
    }

    if (!something_happened)
    {
      // yield to scheduler if nothing happened to allow background
      // threads to run ASAPt 
      sched_yield();
    }
  }

  stop_executors(exec, 1);

  return TURBINE_SUCCESS;
}

/*
 * Get tasks from adlb and execute them.
 * TODO: currently only executes one task, but could do multiple
 * got_tasks: set to true if got at least one task, unmodified otherwise
 */
static turbine_exec_code
get_tasks(Tcl_Interp *interp, turbine_executor *executor,
          int adlb_work_type, void *buffer, size_t buffer_size,
          bool poll, int max_tasks, bool *got_tasks)
{
  adlb_code ac;
  int rc;

  int work_len, answer_rank, type_recved;
  bool got_work;
  if (poll)
  {
    // TODO: move to ADLB_Amget() once ready
    ac = ADLB_Iget(adlb_work_type, buffer, &work_len,
                    &answer_rank, &type_recved);
    EXEC_ADLB_CHECK_MSG(ac, TURBINE_EXEC_OTHER,
                        "Error getting work from ADLB");

    got_work = (ac != ADLB_NOTHING);
  }
  else
  {
    MPI_Comm tmp_comm;
    ac = ADLB_Get(adlb_work_type, buffer, &work_len,
                    &answer_rank, &type_recved, &tmp_comm);
    if (ac == ADLB_SHUTDOWN)
    {
      return TURBINE_EXEC_SHUTDOWN;
    }
    EXEC_ADLB_CHECK_MSG(ac, TURBINE_EXEC_OTHER,
                        "Error getting work from ADLB");
    
    got_work = true;
  }
  
  if (got_work)
  {
    int cmd_len = work_len - 1;
    rc = Tcl_EvalEx(interp, buffer, cmd_len, 0);
    if (rc != TCL_OK)
    {
      launch_error(interp, executor, rc, buffer);
      return TURBINE_ERROR_EXTERNAL;
    }
  }

  turbine_service_finalize();

  return TURBINE_EXEC_SUCCESS;
}

static void
launch_error(Tcl_Interp* interp, turbine_executor *exec, int tcl_rc,
             const char *command)
{
  if (tcl_rc != TCL_ERROR)
  {
    printf("WARNING: Unexpected return code when running task for "
           "executor %s: %d", exec->name, tcl_rc);
  }

  // Pass error to calling script
  char* msg;
  int rc = asprintf(&msg, "Turbine %s worker task error in: %s",
                           exec->name, command);
  assert(rc != -1);
  Tcl_AddErrorInfo(interp, msg);
  free(msg);
}

static void
callback_error(Tcl_Interp* interp, turbine_executor *exec, int tcl_rc,
               Tcl_Obj *command)
{
  if (tcl_rc != TCL_ERROR)
  {
    printf("WARNING: Unexpected return code when running task for "
           "executor %s: %d", exec->name, tcl_rc);
  }

  // Pass error to calling script
  char* msg;
  int rc = asprintf(&msg, "Turbine %s worker task error in callback: %s",
                           exec->name, Tcl_GetString(command));
  assert(rc != -1);
  Tcl_AddErrorInfo(interp, msg);
  free(msg);
}

/*
  task_completed: set to true if at least one task completed,
                  unmodified otherwise
 */
static turbine_exec_code
check_tasks(Tcl_Interp *interp, turbine_executor *executor, bool poll,
            bool *task_completed)
{
  turbine_exec_code ec;
  
  turbine_completed_task completed[COMPLETED_BUFFER_SIZE];
  int ncompleted = COMPLETED_BUFFER_SIZE; // Pass in size
  if (poll)
  {
    ec = executor->poll(executor->state, completed, &ncompleted);
    EXEC_CHECK(ec);
  }
  else
  {
    ec = executor->wait(executor->state, completed, &ncompleted);
    EXEC_CHECK(ec);
  }

  for (int i = 0; i < ncompleted; i++)
  {
    Tcl_Obj *cb, *succ_cb, *fail_cb;
    succ_cb = completed[i].callbacks.success.code;
    fail_cb = completed[i].callbacks.failure.code;
    cb = (completed[i].success) ? succ_cb : fail_cb;

    if (cb != NULL)
    {
      int rc = Tcl_EvalObjEx(interp, cb, 0);
      if (rc != TCL_OK)
      {
        callback_error(interp, executor, rc, cb);
        return TURBINE_ERROR_EXTERNAL;
      }
    }

    if (succ_cb != NULL)
    {
      Tcl_DecrRefCount(succ_cb);
    }
    
    if (fail_cb != NULL)
    {
      Tcl_DecrRefCount(fail_cb);
    }
  }

  return TURBINE_EXEC_SUCCESS;
}


static void
stop_executors(turbine_executor *executors, int nexecutors)
{
  for (int i = 0; i < nexecutors; i++) {
    turbine_executor *exec = &executors[i];
    assert(exec->stop != NULL);

    turbine_exec_code ec = exec->stop(exec->state);
    if (ec != TURBINE_EXEC_SUCCESS)
    {
      // Only warn about error
      TURBINE_ERR_PRINTF("Error while shutting down %s executor\n",
                         exec->name);
    }
  }
}

static void exec_free_cb(const char *key, void *val)
{
  turbine_executor *exec_ptr = val;
  if (exec_ptr->free != NULL)
  {
    exec_ptr->free(exec_ptr->context);
  }
  free(exec_ptr);
}

turbine_code
turbine_async_exec_finalize(void)
{
  if (executors_table_init)
  {
    table_free_callback(&executors, false, exec_free_cb);

    executors_table_init = false;
  }
  
  executors_init = false;

  return TURBINE_SUCCESS;
}
