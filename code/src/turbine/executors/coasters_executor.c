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
#include "src/turbine/executors/coasters_executor.h"

#include "src/turbine/executors/exec_interface.h"

#include "src/turbine/turbine-checks.h"
#include "src/util/debug.h"

#include <assert.h>
#include <unistd.h>

#include <list2_b.h>

/*
  Coasters context info, e.g. configuration that remains constant
  over entire run.
 */
typedef struct {
  // TODO: actual config info
  int total_slots; // TODO: hardcode slots for now
} coasters_context;

/*
  Information about an active task.
 */
typedef struct {
  turbine_task_callbacks callbacks;
  void *job; // TODO: coasters job
} coasters_active_task;

/*
  State of an executor
 */
typedef struct coasters_state {
  // Overall context
  coasters_context *context;

  // Information about slots available
  turbine_exec_slot_state slots;

  // List with coasters_active_task data
  struct list2_b active_tasks;
} coasters_state;

static turbine_exec_code
coasters_initialize(void *context, void **state);

static turbine_exec_code coasters_shutdown(void *state);

static turbine_exec_code coasters_free(void *context);

static turbine_exec_code
coasters_wait(void *state, turbine_completed_task *completed,
          int *ncompleted);

static turbine_exec_code
coasters_poll(void *state, turbine_completed_task *completed,
          int *ncompleted);

static turbine_exec_code
coasters_slots(void *state, turbine_exec_slot_state *slots);

static turbine_exec_code 
coasters_init_context(coasters_context *context);

static turbine_exec_code
check_completed(coasters_state *state, turbine_completed_task *completed,
               int *ncompleted, bool wait_for_completion);

static turbine_exec_code 
init_coasters_executor(turbine_executor *exec, int adlb_work_type)
{
  turbine_exec_code ec;

  exec->name = COASTERS_EXECUTOR_NAME;
  exec->adlb_work_type = adlb_work_type;
  exec->notif_mode = EXEC_POLLING;

  coasters_context *cx = malloc(sizeof(coasters_context));
  EXEC_MALLOC_CHECK(cx);
  ec = coasters_init_context(cx);
  EXEC_CHECK(ec);
  
  exec->context = cx;

  exec->state = NULL;

  // Initialize all function pointers
  exec->initialize = coasters_initialize;
  exec->shutdown = coasters_shutdown;
  exec->free = coasters_free;
  exec->wait = coasters_wait;
  exec->poll = coasters_poll;
  exec->slots = coasters_slots;

  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code 
coasters_init_context(coasters_context *context)
{
  // TODO: load config, etc
  context->total_slots = 4;
  return TURBINE_EXEC_SUCCESS;
}

turbine_code
coasters_executor_register(int adlb_work_type)
{
  turbine_exec_code ec;
  turbine_executor exec;
  ec = init_coasters_executor(&exec, adlb_work_type);
  TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error initializing Coasters executor");

  ec = turbine_add_async_exec(exec);
  TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error registering Coasters executor with Turbine");

  return TURBINE_SUCCESS;
}

static turbine_exec_code
coasters_initialize(void *context, void **state)
{
  assert(context != NULL);
  coasters_context *cx = context;
  coasters_state *s = malloc(sizeof(coasters_state)); 
  EXEC_MALLOC_CHECK(s);

  s->context = cx;

  s->slots.used = 0;
  s->slots.total = cx->total_slots;

  list2_b_init(&s->active_tasks);

  *state = s;
  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code
coasters_shutdown(void *state)
{
  coasters_state *s = state;
  // TODO: iterate over active tasks?

  struct list2_b_item *node = s->active_tasks.head;
  while (node != NULL)
  {
    coasters_active_task *task = (coasters_active_task*)node->data;
    // TODO: include info on task
    fprintf(stderr, "Coasters task still running at shutdown\n");

    // TODO: free coasters job

    Tcl_Obj *cb = task->callbacks.success.code;
    if (cb != NULL)
    {
      Tcl_DecrRefCount(cb);
    }

    cb = task->callbacks.failure.code;
    if (cb != NULL)
    {
      Tcl_DecrRefCount(cb);
    }
  }
  list2_b_clear(&s->active_tasks); 
  free(s);
  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code
coasters_free(void *context)
{
  assert(context != NULL);
  coasters_context *cx = context;
  
  free(cx);
  
  // TODO: clean up other things?
  return TURBINE_EXEC_SUCCESS;
}

turbine_code
coasters_execute(Tcl_Interp *interp, const turbine_executor *exec,
                 const void *work, int length,
                 turbine_task_callbacks callbacks)
{
  assert(exec != NULL);
  // TODO: alternative task info args
  coasters_state *s = exec->state;
  turbine_condition(s != NULL, TURBINE_ERROR_INVALID,
        "Invalid state for coasters executor");
  assert(s->slots.used < s->slots.total);
  s->slots.used++;

  // Store task data inline in list node
  struct list2_b_item *task_node;
  task_node = list2_b_item_alloc(sizeof(coasters_active_task));
  TURBINE_MALLOC_CHECK(task_node);

  coasters_active_task *task = (coasters_active_task*)task_node->data;

  task->job = NULL; // TODO

  // TODO: pass to coasters executor
  DEBUG_TURBINE("COASTERS: Launched task: %.*s\n", length,
                (const char*)work);

  task->callbacks = callbacks;
  if (callbacks.success.code != NULL)
  {
    Tcl_IncrRefCount(callbacks.success.code);
  }
  
  if (callbacks.failure.code != NULL)
  {
    Tcl_IncrRefCount(callbacks.failure.code);
  }

  // Track active tasks
  list2_b_add_item(&s->active_tasks, task_node);
  
  return TURBINE_SUCCESS;
}

static turbine_exec_code
check_completed(coasters_state *state, turbine_completed_task *completed,
               int *ncompleted, bool wait_for_completion)
{
  int completed_size = *ncompleted;
  assert(completed_size >= 1);

  // TODO: ask coasters what is completed
  // TODO: block if none completed
  *ncompleted = 0;

  state->slots.used -= *ncompleted;

  return TURBINE_EXEC_SUCCESS;
}

/*
 * Must wait for at least one task to completed.
 * Assume that at least one task is in system
 */
static turbine_exec_code
coasters_wait(void *state, turbine_completed_task *completed,
          int *ncompleted)
{
  coasters_state *s = state;

  EXEC_CONDITION(s->slots.used, TURBINE_EXEC_INVALID,
                "Cannot wait if no active coasters tasks");

  turbine_exec_code ec = check_completed(s, completed, ncompleted, true);
  EXEC_CHECK_MSG(ec, "error checking for completed tasks in Coasters "
                     "executor");

  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code
coasters_poll(void *state, turbine_completed_task *completed,
          int *ncompleted)
{
  coasters_state *s = state;
  if (s->slots.used > 0)
  {
    turbine_exec_code ec = check_completed(s, completed, ncompleted, false);
    EXEC_CHECK_MSG(ec, "error checking for completed tasks in Coasters "
                       "executor");
  }
  else
  {
    *ncompleted = 0;
  }
  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code
coasters_slots(void *state, turbine_exec_slot_state *slots)
{
  *slots = ((coasters_state*)state)->slots;
  return TURBINE_EXEC_SUCCESS;
}
