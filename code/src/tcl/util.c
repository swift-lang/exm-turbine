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

#include <tools.h>

#include "src/tcl/util.h"

turbine_code
turbine_tcl_long_array(Tcl_Interp* interp, Tcl_Obj* list, int max,
                      long long* output, int* count)
{
  Tcl_Obj** entry;
  int code = Tcl_ListObjGetElements(interp, list, count, &entry);
  assert(code == TCL_OK);
  assert(*count < max);
  for (int i = 0; i < *count; i++)
  {
    code = Tcl_GetWideIntFromObj(interp, entry[i], &output[i]);
    if (code != TCL_OK)
      return TURBINE_ERROR_NUMBER_FORMAT;
  }
  return TURBINE_SUCCESS;
}

turbine_code
turbine_tcl_string_array(Tcl_Interp* interp, Tcl_Obj* list, int max,
                         char** output, int* count)
{
  Tcl_Obj** entry;
  int code = Tcl_ListObjGetElements(interp, list, count, &entry);
  assert(code == TCL_OK);
  assert(*count < max);
  for (int i = 0; i < *count; i++)
  {
    char* t = Tcl_GetStringFromObj(entry[i], NULL);
    if (code != TCL_OK)
          return TURBINE_ERROR_UNKNOWN;
    output[i] = t;
  }
  return TURBINE_SUCCESS;
}

#define TCL_CONDITION_MSG_MAX 1024

void tcl_condition_failed(Tcl_Interp* interp, Tcl_Obj* command,
                          const char* format, ...)
{
  va_list va;
  va_start(va,format);
  char buffer[TCL_CONDITION_MSG_MAX];
  char* commandname = Tcl_GetStringFromObj(command, NULL);
  char* p = &buffer[0];
  p += sprintf(p, "\n");
  p += sprintf(p, "error: ");
  p += sprintf(p, "%s: ", commandname);
  p += vsprintf(p, format, va);
  p += sprintf(p, "\n\n");
  va_end(va);
  // This error will usually be caught and printed again, but
  // we print it here just to be sure it gets printed at least once
  printf("%s", buffer);
  fflush(stdout);
  Tcl_AddErrorInfo(interp, buffer);
}

void
tcl_set_string(Tcl_Interp* interp, char* name, char* value)
{
  Tcl_Obj* p = Tcl_ObjSetVar2(interp, Tcl_NewStringObj(name, -1),
                              NULL, Tcl_NewStringObj(value, -1), 0);
  valgrind_assert(p != NULL);
}

void
tcl_set_integer(Tcl_Interp* interp, char* name, int value)
{
  Tcl_Obj* p = Tcl_ObjSetVar2(interp, Tcl_NewStringObj(name, -1),
                              NULL, Tcl_NewIntObj(value), 0);
  valgrind_assert(p != NULL);
}

void
tcl_set_long(Tcl_Interp* interp, char* name, long value)
{
  Tcl_Obj* p = Tcl_ObjSetVar2(interp, Tcl_NewStringObj(name, -1),
                              NULL, Tcl_NewLongObj(value), 0);
  valgrind_assert(p != NULL);
}

void
tcl_set_wideint(Tcl_Interp* interp, char* name, int64_t value)
{
  Tcl_Obj* p = Tcl_ObjSetVar2(interp, Tcl_NewStringObj(name, -1),
                              NULL, Tcl_NewWideIntObj(value), 0);
  valgrind_assert(p != NULL);
}


void
tcl_dict_put(Tcl_Interp* interp, Tcl_Obj* dict,
             char* key, Tcl_Obj* value)
{
  Tcl_Obj* k = Tcl_NewStringObj(key, -1);
  int rc = Tcl_DictObjPut(interp, dict, k, value);
  valgrind_assert(rc == TCL_OK);
}

void
tcl_dict_get(Tcl_Interp* interp, Tcl_Obj* dict,
             char* key, Tcl_Obj** value)
{
  Tcl_Obj* k = Tcl_NewStringObj(key, -1);
  int rc = Tcl_DictObjGet(interp, dict, k, value);
  valgrind_assert(rc == TCL_OK);
}

Tcl_Obj*
tcl_list_new(int count, char** strings)
{
  Tcl_Obj* objs[count];
  for (int i = 0; i < count; i++)
    objs[i] = Tcl_NewStringObj(strings[i], -1);

  Tcl_Obj* result = Tcl_NewListObj(count, objs);
  return result;
}

Tcl_Obj*
tcl_list_from_array_ints(Tcl_Interp *interp, int* vals, int count)
{
  Tcl_Obj* result = Tcl_NewListObj(0, NULL);
  if (count > 0)
  {
    for (int i = 0; i < count; i++)
    {
      Tcl_Obj* o = Tcl_NewIntObj(vals[i]);
      Tcl_ListObjAppendElement(interp, result, o);
    }
  }
  return result;
}
