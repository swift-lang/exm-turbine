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
#include "src/turbine/executors/coaster_executor.h"

#include "src/turbine/executors/exec_interface.h"

#include "src/turbine/turbine-checks.h"
#include "src/util/debug.h"

#include <assert.h>
#include <unistd.h>

#include <list2_b.h>

/* Check coaster_rc, if failed print message and return return code.
   TODO: get error info message */
#define COASTER_CHECK(crc, err_rc) {                                  \
  coaster_rc __crc = (crc);                                           \
  turbine_condition(__crc == COASTER_SUCCESS, (err_rc),               \
        "Error in Coaster execution: %s (%s)", "...",                 \
        coaster_rc_string(__crc)); }

/*
  Coasters context info, e.g. configuration that remains constant
  over entire run.
 */
typedef struct {
  char *service_url;
  int total_slots; // TODO: fixed slots count for now
  coaster_settings *settings;
} coaster_context;

/*
  Information about an active task.
 */
typedef struct {
  turbine_task_callbacks callbacks;
  coaster_job *job;
} coaster_active_task;

/*
  State of an executor
 */
typedef struct coaster_state {
  // Overall context
  coaster_context *context;

  // Actual Coasters client
  coaster_client *client;

  // Information about slots available
  turbine_exec_slot_state slots;

  // List with coaster_active_task data
  // TODO: table_lp mapping ID to task?
  struct list2_b active_tasks;
} coaster_state;

static turbine_exec_code
coaster_initialize(void *context, void **state);

static turbine_exec_code coaster_shutdown(void *state);

static turbine_exec_code coaster_free(void *context);

static turbine_exec_code
coaster_wait(void *state, turbine_completed_task *completed,
          int *ncompleted);

static turbine_exec_code
coaster_poll(void *state, turbine_completed_task *completed,
          int *ncompleted);

static turbine_exec_code
coaster_slots(void *state, turbine_exec_slot_state *slots);

static turbine_exec_code 
coaster_init_context(coaster_context *context, const char *service_url,
              const char *settings_str);

static turbine_exec_code
check_completed(coaster_state *state, turbine_completed_task *completed,
               int *ncompleted, bool wait_for_completion);

static turbine_exec_code 
init_coaster_executor(turbine_executor *exec, int adlb_work_type,
                      const char *service_url, const char *settings_str)
{
  turbine_exec_code ec;

  exec->name = COASTERS_EXECUTOR_NAME;
  exec->adlb_work_type = adlb_work_type;
  exec->notif_mode = EXEC_POLLING;

  coaster_context *cx = malloc(sizeof(coaster_context));
  EXEC_MALLOC_CHECK(cx);
  ec = coaster_init_context(cx, service_url, settings_str);
  EXEC_CHECK(ec);
  
  exec->context = cx;

  exec->state = NULL;

  // Initialize all function pointers
  exec->initialize = coaster_initialize;
  exec->shutdown = coaster_shutdown;
  exec->free = coaster_free;
  exec->wait = coaster_wait;
  exec->poll = coaster_poll;
  exec->slots = coaster_slots;

  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code 
coaster_init_context(coaster_context *context, const char *service_url,
      const char *settings_str)
{
  // TODO: load config, etc
  context->total_slots = 4;
  context->service_url = strdup(service_url);
  EXEC_MALLOC_CHECK(context->service_url);
  
  coaster_rc crc = coaster_settings_create(&context->settings);
  COASTER_CHECK(crc, TURBINE_EXEC_OOM);
  
  crc = coaster_settings_parse(context->settings, settings_str);
  COASTER_CHECK(crc, TURBINE_EXEC_OOM);

  return TURBINE_EXEC_SUCCESS;
}

turbine_code
coaster_executor_register(int adlb_work_type, const char *service_url,
                          const char *settings_str)
{
  turbine_exec_code ec;
  turbine_executor exec;
  ec = init_coaster_executor(&exec, adlb_work_type, service_url,
                             settings_str);
  TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error initializing Coasters executor");

  ec = turbine_add_async_exec(exec);
  TURBINE_EXEC_CHECK_MSG(ec, TURBINE_ERROR_EXTERNAL,
               "error registering Coasters executor with Turbine");

  return TURBINE_SUCCESS;
}

static turbine_exec_code
coaster_initialize(void *context, void **state)
{
  assert(context != NULL);
  coaster_context *cx = context;
  coaster_state *s = malloc(sizeof(coaster_state)); 
  EXEC_MALLOC_CHECK(s);

  s->context = cx;

  s->slots.used = 0;
  s->slots.total = cx->total_slots;

  list2_b_init(&s->active_tasks);
  
  coaster_rc crc = coaster_client_start(cx->service_url, &s->client);
  COASTER_CHECK(crc, TURBINE_EXEC_OTHER);

  *state = s;
  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code
coaster_shutdown(void *state)
{
  coaster_state *s = state;
  // TODO: iterate over active tasks?
  
  coaster_rc crc = coaster_client_stop(s->client);
  COASTER_CHECK(crc, TURBINE_EXEC_OTHER);

  struct list2_b_item *node = s->active_tasks.head;
  while (node != NULL)
  {
    coaster_active_task *task = (coaster_active_task*)node->data;
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
coaster_free(void *context)
{
  assert(context != NULL);
  coaster_context *cx = context;
  
  coaster_settings_free(cx->settings);
  free(cx->service_url);
  free(cx);
  
  // TODO: clean up other things?
  return TURBINE_EXEC_SUCCESS;
}

turbine_code
coaster_execute(Tcl_Interp *interp, const turbine_executor *exec,
                coaster_job *job, turbine_task_callbacks callbacks)
{
  coaster_rc crc;

  assert(exec != NULL);
  assert(job != NULL);
  coaster_state *s = exec->state;
  turbine_condition(s != NULL, TURBINE_ERROR_INVALID,
        "Invalid state for coasters executor");
  assert(s->slots.used < s->slots.total);
  s->slots.used++;

  // Store task data inline in list node
  struct list2_b_item *task_node;
  task_node = list2_b_item_alloc(sizeof(coaster_active_task));
  TURBINE_MALLOC_CHECK(task_node);

  crc = coaster_submit(s->client, job);
  COASTER_CHECK(crc, TURBINE_ERROR_EXTERNAL);

  DEBUG_TURBINE("COASTERS: Launched task: %.*s\n", length,
                (const char*)work);

  coaster_active_task *task = (coaster_active_task*)task_node->data;
  task->job = job;
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
check_completed(coaster_state *state, turbine_completed_task *completed,
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
coaster_wait(void *state, turbine_completed_task *completed,
          int *ncompleted)
{
  coaster_state *s = state;

  EXEC_CONDITION(s->slots.used, TURBINE_EXEC_INVALID,
                "Cannot wait if no active coasters tasks");

  turbine_exec_code ec = check_completed(s, completed, ncompleted, true);
  EXEC_CHECK_MSG(ec, "error checking for completed tasks in Coasters "
                     "executor");

  return TURBINE_EXEC_SUCCESS;
}

static turbine_exec_code
coaster_poll(void *state, turbine_completed_task *completed,
          int *ncompleted)
{
  coaster_state *s = state;
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
coaster_slots(void *state, turbine_exec_slot_state *slots)
{
  *slots = ((coaster_state*)state)->slots;
  return TURBINE_EXEC_SUCCESS;
}
