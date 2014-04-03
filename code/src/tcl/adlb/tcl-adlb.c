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

/**
 * Tcl extension for ADLB
 *
 * @author wozniak
 * */

// This file should do some user logging using the c-utils
// logging library - this is because the ADLB C layer cannot
// do that effectively, and these functions are called
// directly as Tcl extension functions

// This file should not do DEBUG logging for data operations
// except for during development of this file - the Turbine and ADLB
// messages are more useful.  This file only packs and unpacks
// calls to the ADLB C layer

#include "config.h"
#include <assert.h>

// strnlen() is a GNU extension: Need _GNU_SOURCE
#define _GNU_SOURCE
#if ENABLE_BGP == 1
// Also need __USE_GNU on the BG/P and on older GCC (4.1, 4.3)
#define __USE_GNU
#endif
#include <string.h>
#include <exm-string.h>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include <tcl.h>
#include <mpi.h>
#include <adlb.h>
#include <adlb-defs.h>
#include <adlb_types.h>
#ifdef ENABLE_XPT
#include <adlb-xpt.h>
#endif

#include <log.h>

#include <memory.h>
#include <table_bp.h>
#include <tools.h>
#include <vint.h>

#include "src/tcl/util.h"
#include "src/util/debug.h"

#include "tcl-adlb.h"

// Auto-detect: Old ADLB or new XLB
#ifdef XLB
#define USE_XLB
#else
#define USE_ADLB
#endif

/** The communicator to use in our ADLB instance */
MPI_Comm adlb_comm;

/** The rank of this process in adlb_comm */
int adlb_comm_rank;

/** Number of workers */
static int workers;

/** Number of servers */
static int servers;

static int am_server;

#ifdef USE_ADLB
static int am_debug_server;
#endif

/** Size of MPI_COMM_WORLD */
static int mpi_size = -1;

/** Rank in MPI_COMM_WORLD */
static int mpi_rank = -1;

/** Communicator for ADLB workers */
static MPI_Comm worker_comm;

/** If the controlling code passed us a communicator, it is here */
long adlb_comm_ptr = 0;

/**
 Large buffer for receiving ADLB payloads, etc.
 */
static char xfer[ADLB_PAYLOAD_MAX];
static const adlb_buffer xfer_buf = {
  .data = xfer, .length = ADLB_PAYLOAD_MAX
};

/**
 Smaller scratch buffer for subscripts, etc.
 */
#define TCL_ADLB_SCRATCH_LEN ADLB_DATA_SUBSCRIPT_MAX
static char tcl_adlb_scratch[TCL_ADLB_SCRATCH_LEN];
static const adlb_buffer tcl_adlb_scratch_buf = {
  .data = tcl_adlb_scratch, .length = TCL_ADLB_SCRATCH_LEN
};

/**
 Free any buffer that isn't tcl_adlb_scratch in data
 */
static void free_non_scratch(adlb_buffer buf)
{
  if (buf.data != NULL &&
      buf.data != tcl_adlb_scratch)
  {
    // Must have been malloced
    free(buf.data);
  }
}

/* Return a pointer to a shared transfer buffer */
char *tcl_adlb_xfer_buffer(uint64_t *buf_size) {
  *buf_size = ADLB_PAYLOAD_MAX;
  return xfer;
}

/**
   Map from binary packed [TD,subscript] to local blob pointers.
   This is not an LRU cache: the user must use blob_free to
   free memory
 */
static table_bp blob_cache;

/**
 * Cache Tcl_Objs for struct field names
 */
static struct {
  Tcl_Obj ***objs;
  int size;
} field_name_objs;

/*
  Represent full type of a data structure
 */
typedef struct {
  int len;
  adlb_data_type *types; /* E.g. container and nested types */
  adlb_type_extra *extras; /* E.g. for struct subtype */
} compound_type;

static void set_namespace_constants(Tcl_Interp* interp);

static int refcount_mode(Tcl_Interp *interp, Tcl_Obj *const objv[],
                          Tcl_Obj* obj, adlb_refcount_type *mode);

static int blob_cache_key(Tcl_Interp *interp, Tcl_Obj *const objv[],
                          adlb_datum_id *id, adlb_subscript *sub,
                          void **key, size_t *key_len, bool *alloced);

static Tcl_Obj *build_tcl_blob(void *data, int length, Tcl_Obj *handle);

static int extract_tcl_blob(Tcl_Interp *interp, Tcl_Obj *const objv[],
                   Tcl_Obj *obj, adlb_blob_t *blob, Tcl_Obj **handle);

static int cache_blob(Tcl_Interp *interp, int objc,
    Tcl_Obj *const objv[], adlb_datum_id id, adlb_subscript sub,
    void *blob);

static int uncache_blob(Tcl_Interp *interp, int objc,
    Tcl_Obj *const objv[], adlb_datum_id id, adlb_subscript sub,
    bool *found_in_cache);

static int blob_cache_finalize(void);

static int
packed_struct_to_tcl_dict(Tcl_Interp *interp, Tcl_Obj *const objv[],
                         const void *data, int length,
                         adlb_type_extra extra, Tcl_Obj **result);
static int
tcl_dict_to_adlb_struct(Tcl_Interp *interp, Tcl_Obj *const objv[],
                         Tcl_Obj *dict, adlb_struct_type struct_type,
                         adlb_struct **result);

static int
packed_multiset_to_list(Tcl_Interp *interp, Tcl_Obj *const objv[],
                         const void *data, int length,
                         adlb_type_extra extra, Tcl_Obj **result);

static int
tcl_list_to_packed_multiset(Tcl_Interp *interp, Tcl_Obj *const objv[],
        const compound_type types, int ctype_pos, Tcl_Obj *list,
        adlb_buffer *output, bool *output_caller_buf, int *output_pos);

static int
packed_container_to_dict(Tcl_Interp *interp, Tcl_Obj *const objv[],
       const void *data, int length,
       adlb_type_extra extra, Tcl_Obj **result);

static int
tcl_dict_to_packed_container(Tcl_Interp *interp, Tcl_Obj *const objv[],
        const compound_type types, int ctype_pos, Tcl_Obj *dict,
        adlb_buffer *output, bool *output_caller_buf, int *output_pos);

static int
get_compound_type(Tcl_Interp *interp, int objc, Tcl_Obj *const objv[],
                int *argpos, compound_type *types);

static void
free_compound_type(compound_type *types);

static inline int
compound_type_next(Tcl_Interp *interp, Tcl_Obj *const objv[],
      const compound_type types, int *ctype_pos,
      adlb_data_type *type, adlb_type_extra *extra);

static int
tcl_obj_to_bin_compound(Tcl_Interp *interp, Tcl_Obj *const objv[],
                const compound_type types,
                Tcl_Obj *obj, const adlb_buffer *caller_buffer,
                adlb_binary_data* result);

static int
tcl_obj_bin_append(Tcl_Interp *interp, Tcl_Obj *const objv[],
        const compound_type types, int ctype_pos,
        Tcl_Obj *obj, bool prefix_len,
        adlb_buffer *output, bool *output_caller_buf,
        int *output_pos);

static int
tcl_obj_bin_append2(Tcl_Interp *interp, Tcl_Obj *const objv[],
        adlb_data_type type, adlb_type_extra extra,
        Tcl_Obj *obj, bool prefix_len,
        adlb_buffer *output, bool *output_caller_buf,
        int *output_pos);

static int ADLB_Parse_Struct_Subscript(Tcl_Interp *interp,
  Tcl_Obj *const objv[],
  const char *str, int length,
  adlb_buffer *buf, adlb_subscript *sub,
  bool *using_caller_buf, bool append);

#define PARSE_STRUCT_SUB(str, len, buf, sub, using_caller_buf, append) \
    ADLB_Parse_Struct_Subscript(interp, objv, str, len, buf, sub, \
                                using_caller_buf, append)

static int append_subscript(Tcl_Interp *interp,
      Tcl_Obj *const objv[], adlb_subscript *sub, adlb_subscript to_append,
      adlb_buffer *buf);

static int field_name_objs_init(Tcl_Interp *interp, Tcl_Obj *const objv[]);
static int field_name_objs_add(Tcl_Interp *interp, Tcl_Obj *const objv[],
      adlb_struct_type type, int field_count,
      const char *const* field_names);
static int field_name_objs_finalize(Tcl_Interp *interp,
                                    Tcl_Obj *const objv[]);

#define DEFAULT_PRIORITY 0

/* current priority for rule */
int ADLB_curr_priority = DEFAULT_PRIORITY;

/** We only free this if we are the outermost MPI communicator */
static bool must_comm_free = false;

#define CHECK_ADLB_STORE(rc, id) {                                      \
  TCL_CONDITION(rc != ADLB_REJECTED,                                    \
                "adlb::store <%"PRId64"> failed: double assign!", id);  \
  TCL_CONDITION(rc == ADLB_SUCCESS,                                     \
                "adlb::store <%"PRId64"> failed!", id);                 \
} 

#define CHECK_ADLB_STORE_SUB(rc, id, sub) {                                  \
  TCL_CONDITION(rc != ADLB_REJECTED, "<%"PRId64">[\"%.*s\"], double assign!",\
                  id, (int)sub.length, (const char*)sub.key);                \
  TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64">[\"%.*s\"], double assign!",\
                  id, (int)sub.length, (const char*)sub.key);                \
}
static int
ADLB_Retrieve_Impl(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[], bool decr);

static int
ADLB_Acquire_Ref_Impl(ClientData cdata, Tcl_Interp *interp,
          int objc, Tcl_Obj *const objv[], bool write_ref,
          adlb_subscript_kind sub_kind);
/**
   usage: adlb::init <servers> <types> [<comm>]?
   Simplified use of ADLB_Init type_vect: just give adlb_init
   a number ntypes, and the valid types will be: [0..ntypes-1]
   If comm is given, run ADLB in that communicator
   Else, run ADLB in a dup of MPI_COMM_WORLD
 */
static int
ADLB_Init_Cmd(ClientData cdata, Tcl_Interp *interp,
              int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc == 3 || objc == 4,
                "adlb::init requires 2 or 3 arguments!");

  mm_init();
  turbine_debug_init();

  int rc;

  int servers;
  rc = Tcl_GetIntFromObj(interp, objv[1], &servers);
  TCL_CHECK(rc);

  int ntypes;
  rc = Tcl_GetIntFromObj(interp, objv[2], &ntypes);
  TCL_CHECK(rc);

  int type_vect[ntypes];
  for (int i = 0; i < ntypes; i++)
    type_vect[i] = i;

  bool ok = table_bp_init(&blob_cache, 16);
  TCL_CONDITION(ok, "Could not initialize blob cache");

  rc = field_name_objs_init(interp, objv);
  TCL_CHECK(rc);

  if (objc == 3)
  {
    // Start with MPI_Init() and MPI_COMM_WORLD
    int argc = 0;
    char** argv = NULL;
    must_comm_free = true;
    rc = MPI_Init(&argc, &argv);
    assert(rc == MPI_SUCCESS);
    MPI_Comm_dup(MPI_COMM_WORLD, &adlb_comm);
  }
  else if (objc == 4)
  {
    rc = Tcl_GetLongFromObj(interp, objv[3], &adlb_comm_ptr);
    TCL_CHECK(rc);
    memcpy(&adlb_comm, (void*) adlb_comm_ptr, sizeof(MPI_Comm));
  }
  else
    assert(false);

  MPI_Comm_size(adlb_comm, &mpi_size);
  workers = mpi_size - servers;
  MPI_Comm_rank(adlb_comm, &mpi_rank);

  if (mpi_rank == 0)
  {
    if (workers <= 0)
      puts("WARNING: No workers");
    // Other configuration information will go here...
  }

  // ADLB_Init(int num_servers, int use_debug_server,
  //           int aprintf_flag, int num_types, int *types,
  //           int *am_server, int *am_debug_server, MPI_Comm *app_comm)
#ifdef USE_ADLB
  rc = ADLB_Init(servers, 0, 0, ntypes, type_vect,
                   &am_server, &am_debug_server, &worker_comm);
#endif
#ifdef USE_XLB
  rc = ADLB_Init(servers, ntypes, type_vect,
                 &am_server, adlb_comm, &worker_comm);
#endif
  if (rc != ADLB_SUCCESS)
    return TCL_ERROR;

  if (! am_server)
    MPI_Comm_rank(worker_comm, &adlb_comm_rank);

  set_namespace_constants(interp);

  Tcl_SetObjResult(interp, Tcl_NewIntObj(ADLB_SUCCESS));
  return TCL_OK;
}

/**
   usage: adlb::declare_struct_type <type id> <type name> <field list>
      where field list is a list of (<field name> <field type>)*
      field type should be the full type of the field, i.e. what you
      would pass to adlb::create.
 */
static int
ADLB_Declare_Struct_Type_Cmd(ClientData cdata, Tcl_Interp *interp,
              int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(4);
  int rc;
  adlb_struct_type type_id;
  const char *type_name;

  rc = Tcl_GetIntFromObj(interp, objv[1], &type_id);
  TCL_CHECK(rc);

  type_name = Tcl_GetString(objv[2]);

  Tcl_Obj **field_list;
  int field_list_len;
  rc = Tcl_ListObjGetElements(interp, objv[3], &field_list_len, &field_list);
  TCL_CHECK(rc);
  int max_field_count = field_list_len / 2;
  adlb_struct_field_type field_types[max_field_count];
  const char *field_names[max_field_count];

  int field_count = 0;
  int field_list_ix = 0;
  while (field_list_ix < field_list_len)
  {
    field_names[field_count] = Tcl_GetString(field_list[field_list_ix++]);
  
    TCL_CONDITION(field_list_ix < field_list_len, "missing type for "
                  "field named %s", field_names[field_count]);
    rc = type_from_array(interp, objv, field_list, field_list_len,
                         &field_list_ix, &field_types[field_count].type,
                         &field_types[field_count].extra);
    TCL_CHECK(rc);
    field_count++;
  }

  adlb_data_code dc = ADLB_Declare_struct_type(type_id, type_name, field_count,
                      field_types, field_names);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Declaring ADLB struct type failed");


  rc = field_name_objs_add(interp, objv, type_id, field_count,
                           field_names);
  TCL_CHECK(rc);
  return TCL_OK;
}

static int field_name_objs_init(Tcl_Interp *interp, Tcl_Obj *const objv[])
{
  field_name_objs.size = 64;
  field_name_objs.objs = malloc((size_t)field_name_objs.size *
                            sizeof(field_name_objs.objs[0]));
  TCL_CONDITION(field_name_objs.objs != NULL,
                "error allocating field names");

  // Init. entries
  for (int i = 0; i < field_name_objs.size; i++)
  {
    field_name_objs.objs[i] = NULL;
  }
  return TCL_OK;
}

static int field_name_objs_add(Tcl_Interp *interp, Tcl_Obj *const objv[],
    adlb_struct_type type, int field_count,
    const char *const* field_names)
{
  if (field_name_objs.size <= type)
  {
    int new_size = field_name_objs.size * 2;
    if (new_size <= type)
    {
      new_size = type;
    }
    Tcl_Obj ***tmp = realloc(field_name_objs.objs, (size_t)new_size *
                                    sizeof(field_name_objs.objs[0]));
    TCL_MALLOC_CHECK(tmp);
    // Initialize to NULL
    for (int i = field_name_objs.size; i < new_size; i++)
    {
      tmp[i] = NULL;
    }
    field_name_objs.objs = tmp;
    field_name_objs.size = new_size;
  }

  field_name_objs.objs[type] = malloc(
        sizeof(field_name_objs.objs[0][0]) * (size_t)field_count);
  TCL_MALLOC_CHECK(field_name_objs.objs[type]);
  
  for (int i = 0; i < field_count; i++)
  {
    Tcl_Obj *name_obj = Tcl_NewStringObj(field_names[i], -1);
    TCL_MALLOC_CHECK(name_obj);
    field_name_objs.objs[type][i] = name_obj;
    Tcl_IncrRefCount(name_obj);
  }
  return TCL_OK;
}

/**
 * Free memory used to keep field object names around.
 * Must be called before ADLB_Finalize
 */
static int field_name_objs_finalize(Tcl_Interp *interp,
                                     Tcl_Obj *const objv[])
{
  if (field_name_objs.objs != NULL)
  {
    for (int i = 0; i < field_name_objs.size; i++)
    {
      Tcl_Obj **name_arr = field_name_objs.objs[i];
      if (name_arr != NULL)
      {
        int field_count;
        adlb_data_code dc = ADLB_Lookup_struct_type(i, NULL,
                                  &field_count, NULL, NULL);
        TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
                      "Error looking up struct type %i", i);
        for (int j = 0; j < field_count; j++)
        {
          Tcl_DecrRefCount(name_arr[j]);
        }
        free(name_arr);
      }
    }

    free(field_name_objs.objs);
  }
  field_name_objs.objs = NULL;
  field_name_objs.size = 0;
  return TCL_OK;
}

static void
set_namespace_constants(Tcl_Interp* interp)
{
  tcl_set_integer(interp, "::adlb::SUCCESS",   ADLB_SUCCESS);
  tcl_set_integer(interp, "::adlb::RANK_ANY",  ADLB_RANK_ANY);
  tcl_set_long(interp,    "::adlb::NULL_ID",   ADLB_DATA_ID_NULL);
}

/**
   Enter server
 */
static int
ADLB_Server_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  if (!am_server)
  {
    printf("adlb::server: This process is not a server!\n");
    return TCL_ERROR;
  }

  DEBUG_ADLB("ADLB SERVER...");
  // Limit ADLB to 100MB
  int max_memory = 100*1024*1024;
#ifdef USE_ADLB
  double logging = 0.0;
  int rc = ADLB_Server(max_memory, logging);
#endif
#ifdef USE_XLB
  int rc = ADLB_Server(max_memory);
#endif

  TCL_CONDITION(rc == ADLB_SUCCESS, "SERVER FAILED");

  return TCL_OK;
}

/**
   usage: no args, returns MPI rank
*/
static int
ADLB_Rank_Cmd(ClientData cdata, Tcl_Interp *interp,
              int objc, Tcl_Obj *const objv[])
{
  Tcl_SetObjResult(interp, Tcl_NewIntObj(mpi_rank));
  return TCL_OK;
}

/**
   usage: no args, returns true if a server, else false
*/
static int
ADLB_AmServer_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  Tcl_SetObjResult(interp, Tcl_NewBooleanObj(am_server));
  return TCL_OK;
}

/**
   usage: no args, returns number of MPI world ranks
*/
static int
ADLB_Size_Cmd(ClientData cdata, Tcl_Interp *interp,
              int objc, Tcl_Obj *const objv[])
{
  Tcl_SetObjResult(interp, Tcl_NewIntObj(mpi_size));
  return TCL_OK;
}

/**
   usage: no args, returns number of servers
*/
static int
ADLB_Servers_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  Tcl_SetObjResult(interp, Tcl_NewIntObj(servers));
  return TCL_OK;
}

/**
   usage: no args, returns number of servers
*/
static int
ADLB_Workers_Cmd(ClientData cdata, Tcl_Interp *interp,
                 int objc, Tcl_Obj *const objv[])
{
  Tcl_SetObjResult(interp, Tcl_NewIntObj(workers));
  return TCL_OK;
}

/**
   usage: no args, barrier for workers
*/
static int
ADLB_Barrier_Cmd(ClientData cdata, Tcl_Interp *interp,
                 int objc, Tcl_Obj *const objv[])
{
  int rc = MPI_Barrier(MPI_COMM_WORLD);
  ASSERT(rc == MPI_SUCCESS);
  return TCL_OK;
}

static int
ADLB_Hostmap_Lookup_Cmd(ClientData cdata, Tcl_Interp *interp,
                        int objc, Tcl_Obj *const objv[])
{
  // This is limited only by the number of ranks a user could
  // conceivably put on a node- getting bigger
  int count = 512;
  int ranks[count];
  int actual;

  char* name = Tcl_GetString(objv[1]);

  adlb_code rc = ADLB_Hostmap_lookup(name, count, ranks, &actual);
  TCL_CONDITION(rc == ADLB_SUCCESS || rc == ADLB_NOTHING,
                "error in hostmap!");
  if (rc == ADLB_NOTHING)
    TCL_RETURN_ERROR("host not found: %s", name);

  Tcl_Obj* items[actual];
  for (int i = 0; i < actual; i++)
    items[i] = Tcl_NewIntObj(ranks[i]);

  Tcl_Obj* result = Tcl_NewListObj(actual, items);
  Tcl_SetObjResult(interp, result);

  return TCL_OK;
}

/**
    Output a list containing the entries of the hostmap
    Note that the Turbine version of this function is different
 */
static int
ADLB_Hostmap_List_Cmd(ClientData cdata, Tcl_Interp *interp,
                      int objc, Tcl_Obj *const objv[])
{
  uint count;
  uint name_max;
  ADLB_Hostmap_stats(&count, &name_max);
  // Extra byte per name for RS
  uint chars = count*(name_max+1);
  char* buffer = malloc(chars * sizeof(char));

  int actual;
  ADLB_Hostmap_list(buffer, chars, 0, &actual);
  assert(actual == count);

  Tcl_Obj* names[count];
  char* p = buffer;
  for (int i = 0; i < count; i++)
  {
    char* t = strchr(p, '\r');
    assert(t != NULL);
    *t = '\0';
    Tcl_Obj* name = Tcl_NewStringObj(p, (int) (t-p));
    names[i] = name;
    p = t+1;
  }

  free(buffer);

  assert(count <= INT_MAX);
  Tcl_Obj* result = Tcl_NewListObj((int)count, names);
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   usage: adlb::put <reserve_rank> <work type> <work unit> <priority>
                                                        <parallelism>
*/
static int
ADLB_Put_Cmd(ClientData cdata, Tcl_Interp *interp,
             int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(6);

  int target_rank;
  int work_type;
  int priority;
  int parallelism;
  Tcl_GetIntFromObj(interp, objv[1], &target_rank);
  Tcl_GetIntFromObj(interp, objv[2], &work_type);
  int cmd_len;
  char* cmd = Tcl_GetStringFromObj(objv[3], &cmd_len);
  Tcl_GetIntFromObj(interp, objv[4], &priority);
  Tcl_GetIntFromObj(interp, objv[5], &parallelism);

  DEBUG_ADLB("adlb::put: target_rank: %i type: %i \"%s\" %i",
             target_rank, work_type, cmd, priority);

  // int ADLB_Put(void *work_buf, int work_len, int reserve_rank,
  //              int answer_rank, int work_type, int work_prio)
  int rc = ADLB_Put(cmd, cmd_len+1, target_rank, adlb_comm_rank,
                    work_type, priority, parallelism);

  ASSERT(rc == ADLB_SUCCESS);
  return TCL_OK;
}

/**
   Special-case put that takes no special arguments
   usage: adlb::spawn <work type> <work unit>
*/
static int
ADLB_Spawn_Cmd(ClientData cdata, Tcl_Interp *interp,
             int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(3);

  int work_type;
  Tcl_GetIntFromObj(interp, objv[1], &work_type);
  int cmd_len;
  char* cmd = Tcl_GetStringFromObj(objv[2], &cmd_len);
  int priority = ADLB_curr_priority;

  DEBUG_ADLB("adlb::spawn: type: %i \"%s\" %i", work_type, cmd, priority);

  int rc = ADLB_Put(cmd, cmd_len+1, ADLB_RANK_ANY, adlb_comm_rank,
                    work_type, priority, 1);

  ASSERT(rc == ADLB_SUCCESS);
  return TCL_OK;
}

/**
   usage: get_priority
 */
static int
ADLB_Get_Priority_Cmd(ClientData cdata, Tcl_Interp *interp,
                 int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(1);
  // Return a tcl int
  // Tcl_SetIntObj doesn't like shared values, but it should be
  // safe in our use case to modify in-place
  Tcl_SetObjResult(interp, Tcl_NewIntObj(ADLB_curr_priority));
  return TCL_OK;
}

/**
   usage: reset_priority
 */
static int
ADLB_Reset_Priority_Cmd(ClientData cdata, Tcl_Interp *interp,
                 int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(1);
  ADLB_curr_priority = DEFAULT_PRIORITY;
  return TCL_OK;
}

/**
   usage: set_priority
 */
static int
ADLB_Set_Priority_Cmd(ClientData cdata, Tcl_Interp *interp,
                 int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);
  int rc, new_prio;
  rc = Tcl_GetIntFromObj(interp, objv[1], &new_prio);
  TCL_CHECK_MSG(rc, "Priority must be integer");
  ADLB_curr_priority = new_prio;
  return TCL_OK;
}

/**
   usage: adlb::get <req_type> <answer_rank>
   Returns the next work unit of req_type or empty string when
   ADLB is done
   Stores answer_rank in given output variable
 */
static int
ADLB_Get_Cmd(ClientData cdata, Tcl_Interp *interp,
             int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(3);

  int req_type;
  int error = Tcl_GetIntFromObj(interp, objv[1], &req_type);
  TCL_CHECK(error);
  Tcl_Obj* tcl_answer_rank_name = objv[2];
  DEBUG_ADLB("adlb::get: type=%i", req_type);

  int work_type;

  char* result = &xfer[0];
#ifdef USE_ADLB
  int work_handle[ADLB_HANDLE_SIZE];
#endif
  int work_len;
  int answer_rank;
  bool found_work = false;
  int rc;

#ifdef USE_ADLB

  int req_types[4];
  int work_prio;

  req_types[0] = req_type;
  req_types[1] = req_types[2] = req_types[3] = -1;

  DEBUG_ADLB("enter reserve: type=%i", req_types[0]);
  rc = ADLB_Reserve(req_types, &work_type, &work_prio,
                    work_handle, &work_len, &answer_rank);
  DEBUG_ADLB("exit reserve");
  if (rc == ADLB_DONE_BY_EXHAUSTION)
  {
    DEBUG_ADLB("ADLB_DONE_BY_EXHAUSTION!");
    result[0] = '\0';
  }
  else if (rc == ADLB_NO_MORE_WORK ) {
    DEBUG_ADLB("ADLB_NO_MORE_WORK!");
    result[0] = '\0';
  }
  else if (rc == ADLB_NO_CURRENT_WORK) {
    DEBUG_ADLB("ADLB_NO_CURRENT_WORK");
    result[0] = '\0';
  }
  else if (rc < 0) {
    DEBUG_ADLB("rc < 0");
    result[0] = '\0';
  }
  else
  {
    DEBUG_ADLB("work is reserved.");
    rc = ADLB_Get_reserved(result, work_handle);
    if (rc == ADLB_NO_MORE_WORK)
    {
      puts("No more work on Get_reserved()!");
      result[0] = '\0';
    }
    else
      found_work = true;
  }
  if (result[0] == '\0')
    answer_rank = -1;
#endif

#ifdef USE_XLB
  MPI_Comm task_comm;
  rc = ADLB_Get(req_type, result, &work_len,
                &answer_rank, &work_type, &task_comm);
  if (rc == ADLB_SHUTDOWN)
  {
    result[0] = '\0';
    work_len = 1;
    answer_rank = ADLB_RANK_NULL;
  }
  turbine_task_comm = task_comm;
#endif

  if (found_work)
    DEBUG_ADLB("adlb::get: %s", (char*) result);

  // Store answer_rank in caller's stack frame
  Tcl_Obj* tcl_answer_rank = Tcl_NewIntObj(answer_rank);
  Tcl_ObjSetVar2(interp, tcl_answer_rank_name, NULL, tcl_answer_rank,
                 EMPTY_FLAG);

  Tcl_SetObjResult(interp, Tcl_NewStringObj(result, work_len - 1));
  return TCL_OK;
}

/**
   usage: adlb::iget <req_type> <answer_rank>
   Returns the next work unit of req_type or
        "ADLB_SHUTDOWN" or "ADLB_NOTHING"
   Stores answer_rank in given output variable
 */
static int
ADLB_Iget_Cmd(ClientData cdata, Tcl_Interp *interp,
             int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(3);

  int req_type;
  int error = Tcl_GetIntFromObj(interp, objv[1], &req_type);
  TCL_CHECK(error);
  Tcl_Obj* tcl_answer_rank_name = objv[2];

  DEBUG_ADLB("adlb::get: type=%i", req_type);

  int work_type;

  char* result = &xfer[0];
  int work_len;
  int answer_rank;

  adlb_code rc = ADLB_Iget(req_type, result, &work_len,
                           &answer_rank, &work_type);
  if (rc == ADLB_SHUTDOWN)
  {
    strcpy(result, "ADLB_SHUTDOWN");
    answer_rank = ADLB_RANK_NULL;
  }
  else if (rc == ADLB_NOTHING)
  {
    strcpy(result, "ADLB_NOTHING");
    answer_rank = ADLB_RANK_NULL;
  }

  DEBUG_ADLB("adlb::iget: %s", result);

  // Store answer_rank in caller's stack frame
  Tcl_Obj* tcl_answer_rank = Tcl_NewIntObj(answer_rank);
  Tcl_ObjSetVar2(interp, tcl_answer_rank_name, NULL, tcl_answer_rank,
                 EMPTY_FLAG);

  Tcl_SetObjResult(interp, Tcl_NewStringObj(result, -1));
  return TCL_OK;
}

/**
   Convert type string to adlb_data_type.
   If extra type info is provided, extra->valid is set to true
 */
static int type_from_string(Tcl_Interp *interp, const char* type_string,
                            adlb_data_type *type, adlb_type_extra *extra)
{

  adlb_code rc = ADLB_Data_string_totype(type_string, type, extra);
  if (rc != ADLB_SUCCESS)
  {
    *type = ADLB_DATA_TYPE_NULL;
    char err[strlen(type_string) + 20];
    sprintf(err, "unknown type name %s!", type_string);
    Tcl_AddErrorInfo(interp, err);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/**
  Extract type info from object.

  Does not return any extra type info, if present
 */
int type_from_obj(Tcl_Interp *interp, Tcl_Obj *const objv[],
                   Tcl_Obj* obj, adlb_data_type *type)
{
  adlb_type_extra extra;
  int rc = type_from_obj_extra(interp, objv, obj, type, &extra);
  TCL_CHECK(rc);
  return TCL_OK;
}

int type_from_obj_extra(Tcl_Interp *interp, Tcl_Obj *const objv[],
         Tcl_Obj* obj, adlb_data_type *type, adlb_type_extra *extra)
{
  const char *type_name = Tcl_GetString(obj);
  TCL_CONDITION(type_name != NULL, "type argument not found!");
  int rc = type_from_string(interp, type_name, type, extra);
  TCL_CHECK(rc);
  return TCL_OK;
}

/**
  Extra type info from argument list, advancing index.
  First consume type name as first arg, then if there is additional info
  needed, e.g. container key/value types, consume that info
 */
int type_from_array(Tcl_Interp *interp, Tcl_Obj *const objv[],
        Tcl_Obj *const array[], int len, int *ix,
        adlb_data_type *type, adlb_type_extra *extra)
{
  int rc;
  // Avoid passing out any uninitialized bytes
  memset(extra, 0, sizeof(*extra));

  adlb_data_type tmp_type;
  rc = type_from_obj_extra(interp, objv, array[(*ix)++], &tmp_type,
                           extra);
  TCL_CHECK(rc);
  *type = tmp_type;

  // Process type-specific params if not already in type extra
  if (!extra->valid)
  {
    switch (*type)
    {
      case ADLB_DATA_TYPE_CONTAINER: {
        TCL_CONDITION(len > *ix + 1,
                      "adlb::create type=container requires "
                      "key and value types!");
        adlb_data_type key_type, val_type;
        rc = type_from_obj(interp, objv, array[(*ix)++], &key_type);
        TCL_CHECK(rc);
        rc = type_from_obj(interp, objv, array[(*ix)++], &val_type);
        TCL_CHECK(rc);
        extra->CONTAINER.key_type = key_type;
        extra->CONTAINER.val_type = val_type;
        extra->valid = true;
        break;
      }
      case ADLB_DATA_TYPE_MULTISET: {
        TCL_CONDITION(len > *ix, "adlb::create type=multiset requires "
                      "member type!");
        adlb_data_type val_type;
        rc = type_from_obj(interp, objv, array[(*ix)++], &val_type);
        TCL_CHECK(rc);
        extra->MULTISET.val_type = val_type;
        extra->valid = true;
        break;
      }
      default:
        break;
    }
  }
  return TCL_OK;
}

/*
  Extract variable create properties
  accept_id: if true, accept id as first element
  objv: arguments, objc: argument count, argstart: start argument
 */
static inline int
extract_create_props(Tcl_Interp *interp, bool accept_id, int argstart,
    int objc, Tcl_Obj *const objv[], adlb_datum_id *id, adlb_data_type *type,
    adlb_type_extra *type_extra, adlb_create_props *props)
{
  int rc;
  int argpos = argstart;
  
  // Avoid passing out any uninitialized bytes
  memset(props, 0, sizeof(*props));
  
  if (accept_id) {
    TCL_CONDITION(objc - argstart >= 2, "adlb::create requires >= 2 args!");
    rc = Tcl_GetADLB_ID(interp, objv[argpos++], id);
    TCL_CHECK_MSG(rc, "adlb::create could not get data id");
  } else {
    TCL_CONDITION(objc - argstart >= 1, "adlb::create requires >= 1 args!");
    *id = ADLB_DATA_ID_NULL;
  }

  // Consume type info from arg list
  rc = type_from_array(interp, objv, objv, objc, &argpos, type, type_extra);
  TCL_CHECK(rc);

  // Process create props if present
  *props = DEFAULT_CREATE_PROPS;
  props->release_write_refs = turbine_release_write_rc_policy(*type);

  if (argpos < objc) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &(props->read_refcount));
    TCL_CHECK_MSG(rc, "adlb::create could not get read_refcount argument");
  }

  if (argpos < objc) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &(props->write_refcount));
    TCL_CHECK_MSG(rc, "adlb::create could not get write_refcount argument");
  }

  if (argpos < objc) {
    int permanent;
    rc = Tcl_GetBooleanFromObj(interp, objv[argpos++], &permanent);
    TCL_CHECK_MSG(rc, "adlb::create could not get permanent argument");
    props->permanent = permanent != 0;
  }

  return TCL_OK;
}

/**
   usage: adlb::create <id> <type> [<extra for type>]
          [ <read_refcount> [ <write_refcount> [ <permanent> ] ] ]
   if <id> is adlb::NULL_ID, returns a newly created id
   @param extra is only used for files and containers
*/
static int
ADLB_Create_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  int rc;
  adlb_datum_id id = ADLB_DATA_ID_NULL;
  adlb_data_type type = ADLB_DATA_TYPE_NULL ;
  adlb_type_extra type_extra = ADLB_TYPE_EXTRA_NULL;
  adlb_create_props props;
  extract_create_props(interp, true, 1, objc, objv,
                       &id, &type, &type_extra, &props);

  adlb_datum_id new_id = ADLB_DATA_ID_NULL;

  switch (type)
  {
    case ADLB_DATA_TYPE_INTEGER:
      rc = ADLB_Create_integer(id, props, &new_id);
      break;
    case ADLB_DATA_TYPE_FLOAT:
      rc = ADLB_Create_float(id, props, &new_id);
      break;
    case ADLB_DATA_TYPE_STRING:
      rc = ADLB_Create_string(id, props, &new_id);
      break;
    case ADLB_DATA_TYPE_BLOB:
      rc = ADLB_Create_blob(id, props, &new_id);
      break;
    case ADLB_DATA_TYPE_REF:
      rc = ADLB_Create_ref(id, props, &new_id);
      break;
    case ADLB_DATA_TYPE_STRUCT: {
      adlb_struct_type struct_t = type_extra.valid ?
              type_extra.STRUCT.struct_type : ADLB_STRUCT_TYPE_NULL;
      rc = ADLB_Create_struct(id, props, struct_t, &new_id);
      break;
    }
    case ADLB_DATA_TYPE_CONTAINER: {
      assert(type_extra.valid);
      rc = ADLB_Create_container(id, type_extra.CONTAINER.key_type,
                    type_extra.CONTAINER.val_type, props, &new_id);
      break;
    }
    case ADLB_DATA_TYPE_MULTISET: {
      assert(type_extra.valid);
      rc = ADLB_Create_multiset(id, type_extra.MULTISET.val_type,
                                props, &new_id);
      break;
    }
    case ADLB_DATA_TYPE_NULL:
    default:
      Tcl_AddErrorInfo(interp,
                       "adlb::create: unknown type!");
      return TCL_ERROR;
      break;

  }

  if (id == ADLB_DATA_ID_NULL) {
    // need to return new ID
    Tcl_Obj* result = Tcl_NewADLB_ID(new_id);
    Tcl_SetObjResult(interp, result);
  }

  TCL_CONDITION(rc == ADLB_SUCCESS, "adlb::create <%"PRId64"> failed!", id);
  return TCL_OK;
}


/**
   usage: adlb::multicreate [list of variable specs]*
   each list contains:
          <type> [<extra for type>]
          [ <read_refcount> [ <write_refcount> [ <permanent> ] ] ]
   returns a list of newly created ids
*/
static int
ADLB_Multicreate_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  int rc;
  int count = objc - 1;
  ADLB_create_spec specs[count];

  for (int i = 0; i < count; i++)
  {
    int n;
    Tcl_Obj **elems;
    rc = Tcl_ListObjGetElements(interp, objv[i + 1], &n, &elems);
    TCL_CONDITION(rc == TCL_OK, "adlb::multicreate arg %i must be list", i);
    ADLB_create_spec *spec = &(specs[i]);
    rc = extract_create_props(interp, false, 0, n, elems, &(spec->id),
              &(spec->type), &(spec->type_extra), &(spec->props));
    TCL_CHECK(rc);
  }

  rc = ADLB_Multicreate(specs, count);
  TCL_CONDITION(rc == ADLB_SUCCESS, "adlb::multicreate failed!");

  // Build list to return
  Tcl_Obj *tcl_ids[count];
  for (int i = 0; i < count; i++) {
    tcl_ids[i] = Tcl_NewADLB_ID(specs[i].id);
  }
  Tcl_SetObjResult(interp, Tcl_NewListObj(count, tcl_ids));
  return TCL_OK;
}

static int
ADLB_Exists_Impl(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[],
                adlb_subscript_kind sub_kind)
{
  int min_args = sub_kind == ADLB_SUB_NONE ? 2 : 3;
  TCL_CONDITION(objc >= min_args,
                "requires at least %i arguments", min_args);
  int rc;

  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[1]));

  int argpos = 2;
  if (sub_kind != ADLB_SUB_NONE)
  {
    rc = ADLB_PARSE_SUB(objv[2], sub_kind, &handle.sub, true, true);
    TCL_CHECK_MSG(rc, "Invalid subscript argument %s",
                      Tcl_GetString(objv[2]));
    argpos = 3;
  }

  adlb_refcounts decr = ADLB_NO_RC;
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                           &decr.read_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                           &decr.write_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }

  TCL_CONDITION(argpos == objc,
                "unexpected trailing args at %ith arg", argpos);

  bool b;
  rc = ADLB_Exists(handle.id, handle.sub.val, &b, decr);
  
  TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64"> failed!", handle.id);

  if (sub_kind != ADLB_SUB_NONE)
    // TODO: support binary subscript
    DEBUG_ADLB("adlb::exists <%"PRId64">[%.*s] => %s", handle.id,
                (int)handle.sub.val.length,
                (const char*)handle.sub.val.key, bool2string(b));
  else
    DEBUG_ADLB("adlb::exists <%"PRId64"> => %s", handle.id, bool2string(b));

  ADLB_PARSE_HANDLE_CLEANUP(&handle);

  Tcl_Obj* result = Tcl_NewBooleanObj(b);
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   usage: adlb::exists <id> [ <read decr> ] [ <write decr> ]
 */
static int
ADLB_Exists_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  return ADLB_Exists_Impl(cdata, interp, objc, objv, ADLB_SUB_NONE);
}

/**
   usage: adlb::exists_sub <id> [<subscript>] [ <read decr> ] [ <write decr> ]
 */
static int
ADLB_Exists_Sub_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  return ADLB_Exists_Impl(cdata, interp, objc, objv, ADLB_SUB_CONTAINER);
}


/**
  Check if a datum is closed.
  If not found, counted as closed
  NOTE: decrements are applied before checking for close
   usage: adlb::closed <id> [ <read decr> ] [ <write decr> ]
 */
static int
ADLB_Closed_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc >= 2, "requires at least 1 argument");
  int rc;

  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[1]));

  int argpos = 2;
  adlb_refcounts decr = ADLB_NO_RC;
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                           &decr.read_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                           &decr.write_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }

  TCL_CONDITION(argpos == objc,
                "unexpected trailing args at %ith arg", argpos);

  adlb_refcounts curr_refcounts;
  rc = ADLB_Get_refcounts(handle.id, &curr_refcounts, decr);
  
  TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64"> failed!", handle.id);

  ADLB_PARSE_HANDLE_CLEANUP(&handle);

  bool closed = curr_refcounts.write_refcount == 0;
  Tcl_Obj* result = Tcl_NewBooleanObj(closed);
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/*
  Convert a tcl object to the ADLB representation.
  own_pointers: whether we want to own any memory allocated

  Note: initialises refcounts to 0
  result: the result
  alloced: whether memory was allocated that must be freed with
           ADLB_Free_storage
 */
int
tcl_obj_to_adlb_data(Tcl_Interp *interp, Tcl_Obj *const objv[],
  adlb_data_type type, adlb_type_extra extra,
  Tcl_Obj *obj, bool own_pointers,
  adlb_datum_storage *result, bool *alloced)
{
  int rc;
  *alloced = false; // Most don't allocate data
  switch (type)
  {
    case ADLB_DATA_TYPE_INTEGER:
      rc = Tcl_GetADLBInt(interp, obj, &result->INTEGER);
      TCL_CHECK_MSG(rc, "adlb extract int from %s failed!", Tcl_GetString(obj));
      return TCL_OK;
    case ADLB_DATA_TYPE_REF:
      rc = Tcl_GetADLB_ID(interp, obj, &result->REF.id);
      TCL_CHECK_MSG(rc, "adlb extract int from %s failed!",
                      Tcl_GetString(obj));
      // init refcounts to zero
      result->REF.read_refs = 0;
      result->REF.write_refs = 0;
        
      return TCL_OK;
    case ADLB_DATA_TYPE_FLOAT:
      rc = Tcl_GetDoubleFromObj(interp, obj, &result->FLOAT);
      TCL_CHECK_MSG(rc, "adlb extract double from %s failed!",
                      Tcl_GetString(obj));
      return TCL_OK;
    case ADLB_DATA_TYPE_STRING:
      result->STRING.value = Tcl_GetStringFromObj(obj, &result->STRING.length);
      TCL_CONDITION(result != NULL, "adlb extract string from %p failed!",
                      obj);
      result->STRING.length++; // Account for null byte
      TCL_CONDITION(result->STRING.length < ADLB_DATA_MAX,
          "adlb: string too long (%i bytes)", result->STRING.length);
      if (own_pointers)
      {
        result->STRING.value = strdup(result->STRING.value);
        TCL_CONDITION(result->STRING.value != NULL,
                      "Error allocating memory");
      }
      return TCL_OK;
    case ADLB_DATA_TYPE_BLOB:
    {
      // Take list-based blob representation
      int rc = extract_tcl_blob(interp, objv, obj, &result->BLOB, NULL);
      TCL_CHECK(rc);
      if (own_pointers)
      {
        assert(result->BLOB.length >= 0);
        void *tmp = malloc((size_t)result->BLOB.length);
        TCL_CONDITION(tmp != NULL, "Error allocating memory");
        memcpy(tmp, result->BLOB.value, (size_t)result->BLOB.length);
        result->BLOB.value = tmp;
      }
      return TCL_OK;
    }
    case ADLB_DATA_TYPE_STRUCT:
    {
      TCL_CONDITION(extra.valid, "Must specify struct type to convert "
                                    "dict to struct");
      int rc = tcl_dict_to_adlb_struct(interp, objv, obj,
             extra.STRUCT.struct_type, &result->STRUCT);
      *alloced = true;
      TCL_CHECK(rc);
      return TCL_OK;
    }
    case ADLB_DATA_TYPE_CONTAINER:
    case ADLB_DATA_TYPE_MULTISET:
        // Containers/multiset packed directly to binary
      TCL_RETURN_ERROR("Type %s should be packed directly to binary\n",
          ADLB_Data_type_tostring(type));
      return TCL_ERROR;   
    default:
      printf("unknown type %i!\n", type);
      return TCL_ERROR;
  }
  return TCL_OK;
}

static void
free_compound_type(compound_type *types)
{
  assert(types != NULL);
  if (types->types != NULL)
  {
    free(types->types);
  }
  if (types->extras != NULL)
  {
    free(types->extras);
  }
}

/* Consume next entry from compound_type */
static inline int
compound_type_next(Tcl_Interp *interp, Tcl_Obj *const objv[],
      const compound_type types, int *ctype_pos,
      adlb_data_type *type, adlb_type_extra *extra)
{
  TCL_CONDITION(*ctype_pos < types.len,
          "Consumed past end of compound type info (%i/%i)",
          *ctype_pos, types.len);

  *type = types.types[*ctype_pos];
  if (types.extras == NULL)
  {
    extra->valid = false;
  }
  else
  {
    *extra =  types.extras[*ctype_pos];
  }
  (*ctype_pos)++;
  return TCL_OK;
}

static int
tcl_obj_to_bin_compound(Tcl_Interp *interp, Tcl_Obj *const objv[],
                const compound_type types,
                Tcl_Obj *obj, const adlb_buffer *caller_buffer,
                adlb_binary_data* result)
{
  adlb_data_code dc;
  int rc;

  adlb_buffer packed;
  int pos = 0;
  bool using_caller_buf;

  // Caller blob needs to own data, so don't provide a static buffer
  dc = ADLB_Init_buf(caller_buffer, &packed, &using_caller_buf, 2048);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error initializing buffer");

  rc = tcl_obj_bin_append(interp, objv, types, 0, obj, false,
                          &packed, &using_caller_buf, &pos);
  TCL_CHECK(rc);

  result->data = result->caller_data = packed.data;
  result->length = pos;
  return TCL_OK;
}

/*
  Append binary representation of Tcl object to buffer
  types: full ADLB type of data for serialization
  ctype_pos: current position into types (in case of nested types).
          This is advanced as type entries are processed.
            
 */
static int
tcl_obj_bin_append(Tcl_Interp *interp, Tcl_Obj *const objv[],
        const compound_type types, int ctype_pos,
        Tcl_Obj *obj, bool prefix_len,
        adlb_buffer *output, bool *output_caller_buf,
        int *output_pos)
{
  int rc;
  adlb_data_type type;
  adlb_type_extra extra;

  rc = compound_type_next(interp, objv, types, &ctype_pos, &type, &extra);
  TCL_CHECK(rc);

  // Some serialization routines know how to append to buffer
  if (ADLB_pack_pad_size(type))
  {
    int start_pos = *output_pos;
    if (prefix_len)
    {
      adlb_data_code dc = ADLB_Resize_buf(output, output_caller_buf,
                                        start_pos + (int)VINT_MAX_BYTES);
      TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error resizing");

      memset(output->data + start_pos, 0, VINT_MAX_BYTES);
      (*output_pos) += (int)VINT_MAX_BYTES;
    }

    if (type == ADLB_DATA_TYPE_CONTAINER)
    {
      rc = tcl_dict_to_packed_container(interp, objv, types, ctype_pos,
                  obj, output, output_caller_buf, output_pos);
      TCL_CHECK(rc);
    }
    else if (type == ADLB_DATA_TYPE_MULTISET)
    {
      rc = tcl_list_to_packed_multiset(interp, objv, types, ctype_pos, obj,
                  output, output_caller_buf, output_pos);
      TCL_CHECK(rc);
    }
    else
    {
      TCL_RETURN_ERROR("Don't know how to incrementally append type: %s",
                        ADLB_Data_type_tostring(type));
    }

    if (prefix_len)
    {
      int packed_len = *output_pos - start_pos - (int)VINT_MAX_BYTES;
      // Add int to spot we reserved
      vint_encode(packed_len, output->data + start_pos);
    }
  }
  else
  {
    // In other cases, we serialize the whole thing, then append it
    adlb_datum_storage tmp;
    bool alloced;
    rc = tcl_obj_to_adlb_data(interp, objv, type, extra, obj, false,
                              &tmp, &alloced);
    TCL_CHECK(rc);
  
    adlb_binary_data packed;
    // Make sure data is serialized in contiguous memory
    adlb_data_code dc = ADLB_Pack(&tmp, type, NULL, &packed);

    if (alloced)
    {
      // Free memory before checking for errors
      adlb_data_code dc2 = ADLB_Free_storage(&tmp, type);
      TCL_CONDITION(dc2 == ADLB_DATA_SUCCESS, "Error freeing storage");
    }

    TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
                  "Error packing data type %i into buffer", type);

    dc = ADLB_Append_buffer(ADLB_DATA_TYPE_NULL, packed.data, packed.length,
              prefix_len, output, output_caller_buf, output_pos);

    if (packed.caller_data != NULL)
    {
      // We were given ownership of data, free now
      free(packed.caller_data);
    }
    TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error resizing buffer");

  }
  return TCL_OK;
}

static int
tcl_obj_bin_append2(Tcl_Interp *interp, Tcl_Obj *const objv[],
        adlb_data_type type, adlb_type_extra extra,
        Tcl_Obj *obj, bool prefix_len,
        adlb_buffer *output, bool *output_caller_buf,
        int *output_pos)
{
  // NOTE: it's ok to remove const qualifier since it isn't
  //       modified by called function.
  compound_type ct = { .len = 1, .types = &type,
        .extras = (adlb_type_extra*)&extra };
  return tcl_obj_bin_append(interp, objv, ct, 0, obj,
             false, output, output_caller_buf, output_pos);
}

/**
  Take a Tcl object and an ADLB type and extract the binary representation
  type: adlb data type code
  caller_buffer: optional static buffer to use
  result: serialized result data.  Either has malloced buffer,
          or pointer to caller_buffer->data
 */
int
tcl_obj_to_bin(Tcl_Interp *interp, Tcl_Obj *const objv[],
                adlb_data_type type, adlb_type_extra extra,
                Tcl_Obj *obj, const adlb_buffer *caller_buffer,
                adlb_binary_data* result)
{
  int rc;
  adlb_data_code dc;
  if (type == ADLB_DATA_TYPE_CONTAINER ||
      type == ADLB_DATA_TYPE_MULTISET)
  {
    // For container types, use temporary buffer to append
    adlb_buffer buf;
    bool using_caller_buf;
    int pos = 0;
    dc = ADLB_Init_buf(caller_buffer, &buf,
                                      &using_caller_buf, 128);
    TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error initializing buffer");
    
    rc = tcl_obj_bin_append2(interp, objv, type, extra, obj,
                            false, &buf, &using_caller_buf, &pos);
    TCL_CHECK(rc);

    result->data = result->caller_data = buf.data;
    result->length = pos;
    return TCL_OK;
  }

  // For other types, where we will not typically be appending to array
  adlb_datum_storage tmp;
  bool alloced;
  rc = tcl_obj_to_adlb_data(interp, objv, type, extra, obj, false,
                            &tmp, &alloced);
  TCL_CHECK(rc);

  // Make sure data is serialized in contiguous memory
  dc = ADLB_Pack(&tmp, type, caller_buffer, result);

  if (alloced)
  {
    // Free memory before checking for errors
    adlb_data_code dc2 = ADLB_Free_storage(&tmp, type);
    TCL_CONDITION(dc2 == ADLB_DATA_SUCCESS, "Error freeing storage");
  }

  TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
                "Error packing data type %i into buffer", type);

  // Make sure caller owns the memory (i.e. it's not a pointer to tmp)
  dc = ADLB_Own_data(caller_buffer, result);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
                "Error getting ownership of buffer for data type %i", type);
  return TCL_OK;
}

static int
tcl_dict_to_packed_container(Tcl_Interp *interp, Tcl_Obj *const objv[],
        const compound_type types, int ctype_pos, Tcl_Obj *dict,
        adlb_buffer *output, bool *output_caller_buf, int *output_pos)
{
  int rc;
  adlb_data_code dc;

  int entries;
  rc = Tcl_DictObjSize(interp, dict, &entries);
  TCL_CHECK(rc);

  adlb_data_type key_type, val_type;
  adlb_type_extra key_extra, val_extra;

  // Note: assuming key isn't a compound type, because we don't
  //       consume additional type info for key
  rc = compound_type_next(interp, objv, types, &ctype_pos,
                          &key_type, &key_extra);
  TCL_CHECK(rc);
 
  // Val might be a compound type: we consume that info later
  rc = compound_type_next(interp, objv, types, &ctype_pos,
                          &val_type, &val_extra);
  TCL_CHECK(rc);

  dc = ADLB_Pack_container_hdr(entries, key_type, val_type, output,
                                output_caller_buf, output_pos);
  TCL_CONDITION_GOTO(dc == ADLB_DATA_SUCCESS, exit_err,
        "Error constructing Tcl object for packed container val");

  Tcl_DictSearch iter;

  for (int i = 0; i < entries; i++)
  {
    Tcl_Obj *key, *val;
    int done;
    if (i == 0)
    {
      rc = Tcl_DictObjFirst(interp, dict, &iter, &key, &val, &done);
      TCL_CHECK_MSG_GOTO(rc, exit_err, "Error parsing packed container entry");
    }
    else
    {
      Tcl_DictObjNext(&iter, &key, &val, &done);
    }
    assert(!done); // Should match Tcl_DictObjSize call

    const void *key_data;
    int key_strlen;
    key_data = Tcl_GetStringFromObj(key, &key_strlen);

    // Pack string as binary directly
    dc = ADLB_Append_buffer(key_type, key_data, key_strlen + 1,
                    true, output, output_caller_buf, output_pos); 
    TCL_CONDITION_GOTO(dc == ADLB_DATA_SUCCESS, exit_err,
                       "Error appending to buffer");
    
    // Recursively serialize value (which may be a compound type such as
    //  a list or a dict)
    // Value type needs to be first for recursive call
    int rec_ctype_pos = ctype_pos - 1;
    rc = tcl_obj_bin_append(interp, objv, types, rec_ctype_pos,
                val, true, output, output_caller_buf, output_pos);
    TCL_CHECK_MSG_GOTO(rc, exit_err, "Error serializing dict val");
  }

  return TCL_OK;
exit_err:
  return TCL_ERROR;
}

int
tcl_list_to_packed_multiset(Tcl_Interp *interp, Tcl_Obj *const objv[],
        const compound_type types, int ctype_pos,
        Tcl_Obj *list, adlb_buffer *output, bool *output_caller_buf,
        int *output_pos)
{
  int rc;
  adlb_data_code dc;

  int listc;
  Tcl_Obj **listv;
  rc = Tcl_ListObjGetElements(interp, list, &listc, &listv);
  TCL_CHECK(rc);
  
  
  adlb_data_type elem_type;
  adlb_type_extra elem_extra;

  // Elem might be a compound type: we consume that info later
  rc = compound_type_next(interp, objv, types, &ctype_pos,
                          &elem_type, &elem_extra);
  TCL_CHECK(rc);

  dc = ADLB_Pack_multiset_hdr(listc, elem_type, output, output_caller_buf,
                              output_pos);
  TCL_CONDITION_GOTO(dc == ADLB_DATA_SUCCESS, exit_err,
                     "Error serializing multiset header");
  
  for (int i = 0; i < listc; i++)
  {
    Tcl_Obj *elem = listv[i];
    
    // Value type needs to be first for recursive call
    int rec_ctype_pos = ctype_pos - 1;
    rc = tcl_obj_bin_append(interp, objv, types, rec_ctype_pos,
                elem, true, output, output_caller_buf, output_pos);
    TCL_CHECK_MSG_GOTO(rc, exit_err, "Error serializing multiset elem");
  }
  
  return TCL_OK;

exit_err:
  return TCL_ERROR;
}

/*
   Build a representation of an ADLB struct using Tcl dicts, handling
   nested structs. E.g.

   ADLB struct:
     [ a: { foo: 1, bar: "hello" }, b: 3.14 ]
   Tcl Dict:
     { a: { foo: 1, bar: "hello" }, b: 3.14 }

    If extra type info is provided, checks type is as expected
 */
static int
packed_struct_to_tcl_dict(Tcl_Interp *interp, Tcl_Obj *const objv[],
                         const void *data, int length,
                         adlb_type_extra extra, Tcl_Obj **result)
{
  assert(data != NULL);
  assert(length >= 0);
  assert(result != NULL);
  int rc;

  adlb_struct_type st;

  adlb_packed_struct_hdr *hdr = (adlb_packed_struct_hdr *)data;

  TCL_CONDITION(length >= sizeof(*hdr), "Not enough data for header");

  st = hdr->type;
  TCL_CONDITION(!extra.valid || st == extra.STRUCT.struct_type,
                "Expected struct type %i but got %i",
                extra.STRUCT.struct_type, st);

  const char *st_name;
  int field_count;
  const adlb_struct_field_type *field_types;
  char const* const* field_names;
  adlb_data_code dc = ADLB_Lookup_struct_type(st,
                  &st_name, &field_count, &field_types, &field_names);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
                "Error looking up struct type %i", st);

  TCL_CONDITION(length >= sizeof(*hdr) + sizeof(hdr->field_offsets[0]) *
                (size_t)field_count, "Not enough data for header");

  assert(st < field_name_objs.size);
  Tcl_Obj **field_names2 = field_name_objs.objs[st];
  assert(field_names2 != NULL);

  Tcl_Obj *result_dict = Tcl_NewDictObj();

  for (int i = 0; i < field_count; i++)
  {
    const char *name = field_names[i];
    // Find slice of buffer for the field
    int offset = hdr->field_offsets[i];
    TCL_CONDITION(offset >= 0,
        "invalid struct buffer: negative offset %i for field %s", offset, name);
    // Check if 
    bool valid = (((char*)data)[offset]) != 0;
    if (valid)
    {
      int data_offset = offset + 1;
      const void *field_data = data + data_offset;
      int field_data_length;
      if (i == field_count - 1)
        field_data_length = (int)(length - data_offset);
      else
        field_data_length = hdr->field_offsets[i + 1] - data_offset;

      TCL_CONDITION(field_data_length >= 0,
          "invalid struct buffer: negative length %i for field %s",
                                          field_data_length, name);
      TCL_CONDITION(data_offset + field_data_length <= length,
          "invalid struct buffer: field %s past buffer end: %d+%d vs %d",
          name, data_offset, field_data_length, length);

      // Create a TCL object for the field data
      Tcl_Obj *field_tcl_obj;
      rc = adlb_data_to_tcl_obj(interp, objv, ADLB_DATA_ID_NULL,
                    field_types[i].type, field_types[i].extra,
                    field_data, field_data_length, &field_tcl_obj);
      TCL_CHECK_MSG(rc, "Error building tcl object for field %s", name);

      // Add it to nested dicts
      assert(field_names2[i] != NULL);
      assert(field_tcl_obj != NULL);
      rc = Tcl_DictObjPut(interp, result_dict,
                        field_names2[i], field_tcl_obj);
      TCL_CHECK_MSG(rc, "Error inserting tcl object for field %s", name);
    }
  }

  *result = result_dict;
  return TCL_OK;
}

/*
  Note that result must be freed by caller
 */
static int
tcl_dict_to_adlb_struct(Tcl_Interp *interp, Tcl_Obj *const objv[],
                         Tcl_Obj *dict, adlb_struct_type struct_type,
                         adlb_struct **result)
{
  int rc;
  
  const char *st_name;
  int field_count;
  const adlb_struct_field_type *field_types;
  char const* const* field_names;
  adlb_data_code dc = ADLB_Lookup_struct_type(struct_type,
                  &st_name, &field_count, &field_types, &field_names);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
                "Error looking up struct type %i", struct_type);
  *result = malloc(sizeof(adlb_struct) +
                   sizeof((*result)->fields[0]) * (size_t)field_count);
  TCL_MALLOC_CHECK(*result);
  (*result)->type = struct_type;

  // Get field name objects
  assert(struct_type < field_name_objs.size);
  Tcl_Obj **field_names2 = field_name_objs.objs[struct_type];
  assert(field_names2 != NULL);


  for (int i = 0; i < field_count; i++)
  {
    Tcl_Obj *val;

    rc = Tcl_DictObjGet(interp, dict, field_names2[i], &val);
    TCL_CHECK_MSG(rc, "Could not find val for %s (or %s) in %s",
          field_names[i], Tcl_GetString(field_names2[i]), Tcl_GetString(dict));
    
    if (val != NULL)
    {
      adlb_datum_storage *field = &(*result)->fields[i].data;
      bool alloced;
      // Need to own memory in allocated object so we can free correctly
      rc = tcl_obj_to_adlb_data(interp, objv, field_types[i].type,
                        field_types[i].extra, val, true, field, &alloced);
      TCL_CHECK(rc);
      (*result)->fields[i].initialized = true;
    }
    else
    {
      // Data not present
      (*result)->fields[i].initialized = false;
    }
  }

  return TCL_OK;
}

static int
packed_container_to_dict(Tcl_Interp *interp, Tcl_Obj *const objv[],
                         const void *data, int length,
                         adlb_type_extra extra, Tcl_Obj **result)
{
  int pos = 0;
  adlb_data_type key_type, val_type;
  int entries;
  int rc = TCL_OK;

  adlb_data_code dc;

  dc = ADLB_Unpack_container_hdr(data, length, &pos, &entries,
                                 &key_type, &val_type);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error parsing packed data header");

  if (extra.valid)
  {
    TCL_CONDITION(val_type == extra.CONTAINER.val_type, "Packed value "
          "type doesn't match expected: %s vs. %s",
          ADLB_Data_type_tostring(val_type),
          ADLB_Data_type_tostring(extra.CONTAINER.val_type));
    TCL_CONDITION(key_type == extra.CONTAINER.key_type, "Packed key "
          "type doesn't match expected: %s vs. %s",
          ADLB_Data_type_tostring(key_type),
          ADLB_Data_type_tostring(extra.CONTAINER.key_type));
  }

  Tcl_Obj *dict = Tcl_NewDictObj();
  for (int i = 0; i < entries; i++)
  {
    const void *key, *val;
    int key_len, val_len;
    dc = ADLB_Unpack_container_entry(key_type, val_type, data, length, &pos,
                                &key, &key_len, &val, &val_len);
    TCL_CONDITION_GOTO(dc == ADLB_DATA_SUCCESS, exit_err,
            "Error parsing packed container entry");
    
    Tcl_Obj *key_obj, *val_obj;
    // TODO: interpreting key as string; support binary keys
    
    rc = adlb_data_to_tcl_obj(interp, objv, ADLB_DATA_ID_NULL, val_type,
            ADLB_TYPE_EXTRA_NULL, val, val_len, &val_obj);
    TCL_CHECK_MSG_GOTO(rc, exit_err,
            "Error constructing Tcl object for packed container val");
    
    key_obj = Tcl_NewStringObj(key, key_len - 1);
    rc = Tcl_DictObjPut(interp, dict, key_obj, val_obj);
    if (rc != TCL_OK)
    {
      Tcl_DecrRefCount(key_obj);
      Tcl_DecrRefCount(val_obj);
      tcl_condition_failed(interp, objv[0], 
            "Error adding entry to dict");
      goto exit_err;

    }
  }

  TCL_CONDITION_GOTO(pos == length, exit_err, "Didn't consume all "
        "container data: %i bytes packed, consumed %i bytes", length, pos);

  rc = TCL_OK;
exit_err:
  if (rc == TCL_OK)
  {
    *result = dict;
  }
  else
  {
    Tcl_DecrRefCount(dict);
  }

  return rc;
}

static int
packed_multiset_to_list(Tcl_Interp *interp, Tcl_Obj *const objv[],
                         const void *data, int length,
                         adlb_type_extra extra, Tcl_Obj **result)
{
  Tcl_Obj **arr = NULL;
  int pos = 0;
  adlb_data_type elem_type;
  int entry = 0; // Track how many entries we've inserted
  int entries;
  int rc = TCL_OK;

  adlb_data_code dc;

  dc = ADLB_Unpack_multiset_hdr(data, length, &pos, &entries, &elem_type);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error parsing packed data header");

  if (extra.valid)
  {
    TCL_CONDITION(elem_type == extra.MULTISET.val_type, "Packed element "
          "type doesn't match expected: %s vs. %s",
          ADLB_Data_type_tostring(elem_type),
          ADLB_Data_type_tostring(extra.MULTISET.val_type));
  }


  assert(entries >= 0); 
  arr = malloc(sizeof(Tcl_Obj*) * (size_t)entries);
  for (entry = 0; entry < entries; entry++)
  {
    const void *elem;
    int elem_len;
    dc = ADLB_Unpack_multiset_entry(elem_type, data, length, &pos,
                                    &elem, &elem_len);
    if (dc != ADLB_DATA_SUCCESS)
    {
      tcl_condition_failed(interp, objv[0], 
            "Error parsing packed multiset entry");
      goto exit_err;
    }

    rc = adlb_data_to_tcl_obj(interp, objv, ADLB_DATA_ID_NULL, elem_type,
            ADLB_TYPE_EXTRA_NULL, elem, elem_len, &arr[entry]);
    if (rc != TCL_OK)
    {
      tcl_condition_failed(interp, objv[0], 
            "Error constructing Tcl object for packed multiset entry");
      goto exit_err;
    }
  }

  TCL_CONDITION_GOTO(pos == length, exit_err, "Didn't consume all "
        "container data: %i bytes packed, consumed %i bytes", length, pos);
  rc = TCL_OK;
exit_err:
  if (rc == TCL_OK)
  {
    *result = Tcl_NewListObj(entries, arr);
    free(arr);
  }
  else if (arr != NULL)
  {
    // Free any added entries
    for (int i = 0; i < entry - 1; i++)
    {
      Tcl_DecrRefCount(arr[i]);
    }
    free(arr);
  }

  return rc;
}


/**
   usage: adlb::store <id> <type> [ <extra> ] <value>
                      [ <decrement writers> ] [ <decrement readers> ]
                      [ <store readers> ] [ <store writers> ]
   extra: any extra info for type, e.g. struct type when storing struct
   value: value to be stored
   decrement readers/writers: Optional  Decrement the readers/writers
          reference count by this amount.  Defaults are 0 read, 1 write
   store readers/writers: Optional  Add this many references to any
          stored reference variables.   Defaults are 2 read, 0 write
*/
static int
ADLB_Store_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc >= 4, "requires at least 4 args!");
  int rc;
  int argpos = 1;

  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[argpos++], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s",
                Tcl_GetString(objv[argpos-1]));

  adlb_data_type type;
  adlb_type_extra extra;
  rc = type_from_obj_extra(interp, objv, objv[argpos++], &type,
                         &extra);
  TCL_CHECK(rc);

  adlb_binary_data data; // The data to send
  if (type == ADLB_DATA_TYPE_CONTAINER ||
      type == ADLB_DATA_TYPE_MULTISET)
  {
    // Handle non-straightforward cases where we need additional type info
    argpos--; // Rewind so type can be reprocessed
    compound_type compound_type;
    rc = get_compound_type(interp, objc, objv, &argpos, &compound_type);
    TCL_CHECK(rc);

    Tcl_Obj *obj = objv[argpos++];
    // Straightforward case with no nested type info
    rc = tcl_obj_to_bin_compound(interp, objv, compound_type,
                                 obj, &xfer_buf, &data);
    TCL_CHECK_MSG(rc, "<%"PRId64"> failed, could not extract data from %s!",
                  handle.id, Tcl_GetString(obj));
    free_compound_type(&compound_type);
  }
  else
  {
    Tcl_Obj *obj = objv[argpos++];
    // Straightforward case with no nested type info
    rc = tcl_obj_to_bin(interp, objv, type, extra,
                        obj, &xfer_buf, &data);
    TCL_CHECK_MSG(rc, "<%"PRId64"> failed, could not extract data from %s!",
                  handle.id, Tcl_GetString(obj));
  }

  // Handle optional refcount spec
  adlb_refcounts decr = ADLB_WRITE_RC; // default is to decr writers
  if (argpos < objc) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.write_refcount);
    TCL_CHECK_MSG(rc, "decrement arg must be int!");
  }

  if (argpos < objc) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.read_refcount);
    TCL_CHECK_MSG(rc, "decrement arg must be int!");
  }

  // Handle optional number of refcounts to store
  adlb_refcounts store_refcounts = ADLB_READ_RC;
  if (argpos < objc) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                 &store_refcounts.read_refcount);
    TCL_CHECK_MSG(rc, "store refcount arg must be int!");
  }

  if (argpos < objc) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                 &store_refcounts.write_refcount);
    TCL_CHECK_MSG(rc, "store refcount arg must be int!");
  }


  TCL_CONDITION(argpos == objc,
          "extra trailing arguments starting at argument %i", argpos);

  // DEBUG_ADLB("adlb::store: <%"PRId64">=%s", id, data);
  int store_rc = ADLB_Store(handle.id, handle.sub.val, type,
                  data.data, data.length, decr, store_refcounts);
  
  // Free if needed
  if (data.data != xfer_buf.data)
    ADLB_Free_binary_data(&data);
  
  CHECK_ADLB_STORE(store_rc, handle.id);
  
  rc = ADLB_PARSE_HANDLE_CLEANUP(&handle);
  TCL_CHECK(rc);

  return TCL_OK;
}

static inline void report_type_mismatch(adlb_data_type expected,
                                        adlb_data_type actual);

/**
   usage: adlb::retrieve <id> [<type>]
   @param type: if provided, then check that data is of correct type
   returns the contents of the adlb datum converted to a tcl object
*/
static int
ADLB_Retrieve_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  return ADLB_Retrieve_Impl(cdata, interp, objc, objv, false);
}

/**
   usage: adlb::retrieve_decr <id> <decr> [<type>]
   same as retrieve, but also decrement read reference count by <decr>
*/
static int
ADLB_Retrieve_Decr_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  return ADLB_Retrieve_Impl(cdata, interp, objc, objv, true);
}

static int
ADLB_Retrieve_Impl(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[], bool decr)
{
  if (decr) {
    TCL_CONDITION((objc == 3 || objc == 4),
                  "requires 2 or 3 args!");
  } else {
    TCL_CONDITION((objc == 2 || objc == 3),
                  "requires 1 or 2 args!");
  }

  int rc;
  int argpos = 1;

  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[argpos++], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s",
                Tcl_GetString(objv[argpos-1]));

  int decr_amount = 0;
  if (decr) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr_amount);
    TCL_CHECK_MSG(rc, "requires decr amount!");
  }

  adlb_data_type given_type = ADLB_DATA_TYPE_NULL;
  adlb_type_extra extra = { .valid = false };
  if (argpos < objc)
  {
    rc = type_from_obj_extra(interp, objv, objv[argpos++], &given_type,
                             &extra);
    TCL_CHECK_MSG(rc, "arg %i must be valid type!", argpos);
  }

  // Retrieve the data, actual type, and length from server
  adlb_data_type type;
  int length;
  adlb_retrieve_rc refcounts = ADLB_RETRIEVE_NO_RC;
  refcounts.decr_self.read_refcount = decr_amount;
  int ret_rc = ADLB_Retrieve(handle.id, handle.sub.val, refcounts,
                     &type, xfer, &length);

  TCL_CONDITION(ret_rc == ADLB_SUCCESS, "<%"PRId64"> failed!", handle.id);
  TCL_CONDITION(length >= 0, "adlb::retrieve <%"PRId64"> not found!",
                            handle.id);
  
  rc = ADLB_PARSE_HANDLE_CLEANUP(&handle);
  TCL_CHECK(rc);

  // Type check
  if ((given_type != ADLB_DATA_TYPE_NULL &&
       given_type != type))
  {
    report_type_mismatch(given_type, type);
    return TCL_ERROR;
  }

  // Unpack from xfer to Tcl object
  Tcl_Obj* result = NULL;
  rc = adlb_data_to_tcl_obj(interp, objv, handle.id, type, extra,
                            xfer, length, &result);
  TCL_CHECK(rc);

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   interp, objv, id, and length: just for error checking and messages
   If object is a blob, this converts it to a string
 */
int
adlb_data_to_tcl_obj(Tcl_Interp *interp, Tcl_Obj *const objv[], adlb_datum_id id,
                adlb_data_type type, adlb_type_extra extra,
                const void *data, int length, Tcl_Obj** result)
{
  adlb_datum_storage tmp;
  adlb_data_code dc;
  assert(length >= 0);
  assert(length < ADLB_DATA_MAX);

  switch (type)
  {
    case ADLB_DATA_TYPE_INTEGER:
      dc = ADLB_Unpack_integer(&tmp.INTEGER, data, length);
      TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
            "Retrieve failed due to error unpacking data %i", dc);
      *result = Tcl_NewADLBInt(tmp.INTEGER);
      break;
    case ADLB_DATA_TYPE_REF:
      dc = ADLB_Unpack_ref(&tmp.REF, data, length, ADLB_NO_RC);
      TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
            "Retrieve failed due to error unpacking data %i", dc);
      *result = Tcl_NewADLB_ID(tmp.REF.id);
      break;
    case ADLB_DATA_TYPE_FLOAT:
      dc = ADLB_Unpack_float(&tmp.FLOAT, data, length);
      TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
            "Retrieve failed due to error unpacking data %i", dc);
      *result = Tcl_NewDoubleObj(tmp.FLOAT);
      break;
    case ADLB_DATA_TYPE_STRING:
      // Don't allocate new memory
      // Ok to cast away const since TCL will copy string anyway
      dc = ADLB_Unpack_string(&tmp.STRING, (void *)data,
                              length, false);
      TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
            "Retrieve failed due to error unpacking data %i", dc);
      *result = Tcl_NewStringObj(tmp.STRING.value, tmp.STRING.length-1);
      break;
    case ADLB_DATA_TYPE_BLOB:
      // Do allocate new memory
      // Ok to cast away const since we're copying blob
      dc = ADLB_Unpack_blob(&tmp.BLOB, (void *)data, length, true);
      TCL_CONDITION(dc == ADLB_DATA_SUCCESS,
            "Retrieve failed due to error unpacking data %i", dc);
      // Don't provide id to avoid blob caching
      *result = build_tcl_blob(tmp.BLOB.value, tmp.BLOB.length, NULL);
      break;
    case ADLB_DATA_TYPE_STRUCT:
      return packed_struct_to_tcl_dict(interp, objv, data, length,
                                       extra, result);
    case ADLB_DATA_TYPE_CONTAINER:
      return packed_container_to_dict(interp, objv, data, length, extra, result);
    case ADLB_DATA_TYPE_MULTISET:
      return packed_multiset_to_list(interp, objv, data, length, extra, result);
    default:
      *result = NULL;
      TCL_CONDITION(false, "unsupported type: %s(%i)",
                           ADLB_Data_type_tostring(type), type);
  }
  return TCL_OK;
}

static inline void
report_type_mismatch(adlb_data_type expected,
                     adlb_data_type actual)
{
  printf("type mismatch: expected: %s - received: %s\n",
                      ADLB_Data_type_tostring(expected),
                      ADLB_Data_type_tostring(actual));
}

/**
   usage: adlb::acquire_ref <id> <type> <increment> <decrement>
   Retrieve and increment read refcount of referenced ids by increment.
   Decrement refcount of this id by decrement
*/
static int
ADLB_Acquire_Ref_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  return ADLB_Acquire_Ref_Impl(cdata, interp, objc, objv,
                               false, ADLB_SUB_NONE);
}

/**
   usage: adlb::acquire_write_ref <id> <type>
          <read increment> <write increment> <read decrement>
   Retrieve and increment read & write refcount of referenced ids by increment.
   Decrement refcount of this id by decrement
*/
static int
ADLB_Acquire_Write_Ref_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  return ADLB_Acquire_Ref_Impl(cdata, interp, objc, objv,
                               true, ADLB_SUB_NONE);
}

/**
   usage: adlb::acquire_sub_ref <id> <subscript> <type> <increment> <decrement>
   Retrieve value at subscript and increment read refcount of referenced
   ids by increment.
   Decrement refcount of this id by decrement
*/
static int
ADLB_Acquire_Sub_Ref_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  return ADLB_Acquire_Ref_Impl(cdata, interp, objc, objv,
                               false, ADLB_SUB_CONTAINER);
}

/**
   usage: adlb::acquire_sub_write_ref <id> <subscript> <type>
          <read increment> <write increment> <read decrement>
   Retrieve value at subscript and increment read & write refcounts
   of referenced ids by increment.
   Decrement refcount of this id by decrement
*/
static int
ADLB_Acquire_Sub_Write_Ref_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  return ADLB_Acquire_Ref_Impl(cdata, interp, objc, objv,
                               true, ADLB_SUB_CONTAINER);
}

static int
ADLB_Acquire_Ref_Impl(ClientData cdata, Tcl_Interp *interp,
          int objc, Tcl_Obj *const objv[],
          bool write_ref, adlb_subscript_kind sub_kind)
{
  int expected_args = 5;
  if (sub_kind != ADLB_SUB_NONE) {
    expected_args++;
  }
  if (write_ref)
  {
    expected_args++;
  }

  TCL_ARGS(expected_args);
  int rc;
  
  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[1]));
  
  int argpos = 2;

  if (sub_kind != ADLB_SUB_NONE)
  {
    rc = ADLB_PARSE_SUB(objv[2], sub_kind, &handle.sub, true, true);
    TCL_CHECK_MSG(rc, "Invalid subscript argument %s",
                      Tcl_GetString(objv[2]));
    argpos = 3;
  }

  adlb_data_type expected_type;
  adlb_type_extra extra;
  rc = type_from_obj_extra(interp, objv, objv[argpos++], &expected_type,
                          &extra);
  TCL_CHECK(rc);

  adlb_retrieve_rc refcounts = ADLB_RETRIEVE_NO_RC;
  rc = Tcl_GetIntFromObj(interp, objv[argpos++],
            &refcounts.incr_referand.read_refcount);
  TCL_CHECK_MSG(rc, "requires incr referand read amount!");

  if (write_ref) {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
              &refcounts.incr_referand.write_refcount);
    TCL_CHECK_MSG(rc, "requires incr referand write amount!");
  }

  rc = Tcl_GetIntFromObj(interp, objv[argpos++],
            &refcounts.decr_self.read_refcount);
  TCL_CHECK_MSG(rc, "requires decr amount!");

  // Retrieve the data, actual type, and length from server
  adlb_data_type type;
  int length;
  rc = ADLB_Retrieve(handle.id, handle.sub.val, refcounts, &type, xfer, &length);
  if (adlb_has_sub(handle.sub.val))
  {
    TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64">[%.*s] failed!", handle.id,
            (int)handle.sub.val.length, (const char*)handle.sub.val.key);
    TCL_CONDITION(length >= 0, "<%"PRId64">[%.*s] not found!", handle.id,
            (int)handle.sub.val.length, (const char*)handle.sub.val.key);
  }
  else
  {
    TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64"> failed!", handle.id);
    TCL_CONDITION(length >= 0, "<%"PRId64"> not found!", handle.id);
  }

  ADLB_PARSE_HANDLE_CLEANUP(&handle);

  // Type check
  if (expected_type != type)
  {
    report_type_mismatch(expected_type, type);
    return TCL_ERROR;
  }

  // Unpack from xfer to Tcl object
  Tcl_Obj* result;
  rc = adlb_data_to_tcl_obj(interp, objv, handle.id, type, extra,
                            xfer, length, &result);
  TCL_CHECK(rc);

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}


static inline int
set_enumerate_params(Tcl_Interp *interp, Tcl_Obj *const objv[],
                     const char* token, bool *include_keys,
                     bool *include_vals);

static inline int
enumerate_object(Tcl_Interp *interp, Tcl_Obj *const objv[],
                      adlb_datum_id id,
                      bool include_keys, bool include_vals,
                      char* data, int length, int records,
                      adlb_type_extra kv_type, Tcl_Obj** result);

/**
   usage:
   adlb::enumerate <id> subscripts|members|dict|count
                   <count>|all <offset> [<read decr>] [<write decr>]

   subscripts: return list of subscript strings
   members: return list of member TDs
   dict: return dict mapping subscripts to TDs
   count: return integer count of container elements
 */
static int
ADLB_Enumerate_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc >= 5, "must have at least 5 arguments");
  int rc;
  int argpos = 1;
  adlb_datum_id container_id;
  int count;
  int offset;
  rc = Tcl_GetADLB_ID(interp, objv[argpos++], &container_id);
  TCL_CHECK_MSG(rc, "requires container id!");
  char* token = Tcl_GetStringFromObj(objv[argpos++], NULL);
  TCL_CONDITION(token, "requires token!");
  // This argument is either the integer count or "all", all == -1

  Tcl_Obj *count_obj = objv[argpos++];
  char* tmp = Tcl_GetStringFromObj(count_obj, NULL);
  if (strcmp(tmp, "all"))
  {
    rc = Tcl_GetIntFromObj(interp, count_obj, &count);
    TCL_CHECK_MSG(rc, "requires count!");
  }
  else
    count = -1;
  rc = Tcl_GetIntFromObj(interp, objv[argpos++], &offset);
  TCL_CHECK_MSG(rc, "requires offset!");

  adlb_refcounts decr = ADLB_NO_RC;
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.read_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.write_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }

  TCL_CONDITION(argpos == objc, "unexpected trailing args at %ith arg", argpos);

  // Set up call
  bool include_keys;
  bool include_vals;
  void *data = NULL;
  int data_length;
  int records;
  adlb_type_extra kv_type;
  rc = set_enumerate_params(interp, objv, token, &include_keys, &include_vals);
  TCL_CHECK_MSG(rc, "unknown token %s!", token);


  // Call ADLB
  rc = ADLB_Enumerate(container_id, count, offset, decr,
                      include_keys, include_vals,
                      &data, &data_length, &records, &kv_type);
  TCL_CONDITION(rc == ADLB_SUCCESS, "ADLB enumerate call failed");

  // Return results to Tcl
  Tcl_Obj* result;
  rc = enumerate_object(interp, objv, container_id,
                        include_keys, include_vals,
                        data, data_length, records, kv_type, &result);
  TCL_CHECK(rc);

  if (data != NULL)
    free(data);

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   Interpret args and set params
   interp, objv provided for error handling
 */
static inline int
set_enumerate_params(Tcl_Interp *interp, Tcl_Obj *const objv[],
                     const char* token, bool *include_keys,
                     bool *include_vals)
{
  if (!strcmp(token, "subscripts"))
  {
    *include_keys = true;
    *include_vals = false;
  }
  else if (!strcmp(token, "members"))
  {
    *include_keys = false;
    *include_vals = true;
  }
  else if (!strcmp(token, "dict"))
  {
    *include_keys = true;
    *include_vals = true;
  }
  else if (!strcmp(token, "count"))
  {
    *include_keys = false;
    *include_vals = false;
  }
  else
  {
    return TCL_ERROR;
  }
  return TCL_OK;
}

/**
   Simple string struct for indices of strings
   Note: s may not be NULL-terminated: user must refer to length
 */
struct record_entry
{
  char* s;
  int length;
};

/**
   Pack ADLB_Enumerate results into Tcl object
 */
static inline int
enumerate_object(Tcl_Interp *interp, Tcl_Obj *const objv[],
                      adlb_datum_id id,
                      bool include_keys, bool include_vals,
                      char* data, int length, int records,
                      adlb_type_extra kv_type, Tcl_Obj** result)
{
  int rc;
  int list_buf_len = 0;
  if (include_keys && include_vals)
  {
    *result = Tcl_NewDictObj();
  }
  else if (include_keys || include_vals)
  {
    // Create list at end
    *result = NULL;
    list_buf_len = records;
  }
  else
  {
    // Just return count
    *result = Tcl_NewIntObj(records);
    return TCL_OK;
  }

  // Buffer for list
  Tcl_Obj * list_buf[list_buf_len];

  // Position in buffer
  int pos = 0;
  int consumed; // Amount just consumed

  for (int i = 0; i < records; i++)
  {
    Tcl_Obj *key = NULL, *val = NULL;
    if (include_keys)
    {
      int64_t key_len;
      consumed = vint_decode(data + pos, length - pos, &key_len);
      TCL_CONDITION(consumed >= 1, "Corrupted message received: bad key "
                    "length for record %i/%i", i+1, records);
      pos += consumed;
      TCL_CONDITION(key_len <= length - pos, "Truncated/corrupted "
            "message received, key for record %i/%i extends beyond end "
            "of data", i + 1, records);
      // Key currently must be string
      // TODO: support binary key
      key = Tcl_NewStringObj(data + pos, (int)key_len - 1);
      pos += (int)key_len;
    }

    if (include_vals)
    {
      int64_t val_len;
      consumed = vint_decode(data + pos, length - pos, &val_len);
      TCL_CONDITION(consumed >= 1, "Corrupted message received: bad "
            "value length for record %i/%i", i + 1, records);
      pos += consumed;
      TCL_CONDITION(val_len <= length - pos, "Truncated/corrupted "
            "message received, key for record %i/%i extends beyond end "
            "of data", i + 1, records);
      rc = adlb_data_to_tcl_obj(interp, objv, id, kv_type.CONTAINER.val_type,
                ADLB_TYPE_EXTRA_NULL, data + pos, (int)val_len, &val);

      pos += (int)val_len;
    }

    if (include_keys && include_vals)
    {
      rc = Tcl_DictObjPut(interp, *result, key, val);
      TCL_CHECK(rc);
    }
    else if (include_keys)
    {
      list_buf[i] = key;
    }
    else
    { assert(include_vals);
      list_buf[i] = val;
    }
  }

  if (!include_keys || !include_vals)
  {
    // Build list from elements
    *result = Tcl_NewListObj(records, list_buf);
  }

  return TCL_OK;
}

static inline int
ADLB_Retrieve_Blob_Impl(ClientData cdata, Tcl_Interp *interp,
                        int objc, Tcl_Obj *const objv[], bool decr);

/**
   Copy a blob from the distributed store into a local blob
   in the memory of this process
   Must be freed with adlb::blob_free
   usage: adlb::retrieve_blob <id> => [ list <pointer> <length> ]
 */
static int
ADLB_Retrieve_Blob_Cmd(ClientData cdata, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
  return ADLB_Retrieve_Blob_Impl(cdata, interp, objc, objv, false);
}

static int
ADLB_Retrieve_Blob_Decr_Cmd(ClientData cdata, Tcl_Interp *interp,
                            int objc, Tcl_Obj *const objv[])
{
  return ADLB_Retrieve_Blob_Impl(cdata, interp, objc, objv, true);
}

/**
 * Construct cache key
 * Key may point to id or sub
 */
static int blob_cache_key(Tcl_Interp *interp, Tcl_Obj *const objv[],
                          adlb_datum_id *id, adlb_subscript *sub,
                          void **key, size_t *key_len, bool *alloced)
{
  if (adlb_has_sub(*sub))
  {
    *key_len = sizeof(*id) + sub->length;
    *key = malloc(*key_len);
    TCL_MALLOC_CHECK(*key);
    *alloced = true;

    memcpy(*key, id, sizeof(*id));
    memcpy(*key + sizeof(*id), sub->key, sub->length);
  }
  else
  {
    *key = id;
    *key_len = sizeof(*id);
    *alloced = false;
  }
  
  return TCL_OK;
}

static inline int
ADLB_Retrieve_Blob_Impl(ClientData cdata, Tcl_Interp *interp,
                        int objc, Tcl_Obj *const objv[], bool decr)
{
  if (decr) {
    TCL_ARGS(3);
  } else {
    TCL_ARGS(2);
  }

  int rc;
  tcl_adlb_handle handle;
  Tcl_Obj *handle_obj = objv[1];
  rc = ADLB_PARSE_HANDLE(handle_obj, &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s",
                Tcl_GetString(objv[1]));

  adlb_retrieve_rc refcounts = ADLB_RETRIEVE_NO_RC;
  /* Only decrement if refcounting enabled */
  if  (decr) {
    rc = Tcl_GetIntFromObj(interp, objv[2],
                          &refcounts.decr_self.read_refcount);
    TCL_CHECK_MSG(rc, "requires id!");
  }

  // Retrieve the blob data
  adlb_data_type type;
  int length;
  int ret_rc = ADLB_Retrieve(handle.id, handle.sub.val, refcounts,
                             &type, xfer, &length);

  TCL_CONDITION(ret_rc == ADLB_SUCCESS, "<%"PRId64"> failed!",
                handle.id);
  TCL_CONDITION(type == ADLB_DATA_TYPE_BLOB,
                "type mismatch: expected: %i actual: %i",
                ADLB_DATA_TYPE_BLOB, type);

  // Allocate the local blob
  void* blob = malloc((size_t)length);
  TCL_CONDITION(blob != NULL, "Error allocating blob: %i bytes", length);

  // Copy the blob data
  memcpy(blob, xfer, (size_t)length);

  DEBUG_ADLB("ADD TO CACHE: {%s}\n", Tcl_GetString(handle_obj));
  rc = cache_blob(interp, objc, objv, handle.id, handle.sub.val, blob);
  TCL_CHECK(rc);

  // printf("retrieved blob: [ %p %i ]\n", blob, length);
  rc = ADLB_PARSE_HANDLE_CLEANUP(&handle);
  TCL_CHECK(rc);
  
  // build blob with original handle - ID or ID/sub
  Tcl_SetObjResult(interp, build_tcl_blob(blob, length, handle_obj));
  return TCL_OK;
}

// Return null on out of memory
static Tcl_Obj *build_tcl_blob(void *data, int length, Tcl_Obj *handle)
{
  // Pack and return the blob pointer, length, turbine ID as Tcl list
  int blob_elems = (handle == NULL) ? 2 : 3;

  Tcl_Obj* list[blob_elems];
  list[0] = Tcl_NewPtr(data);
  list[1] = Tcl_NewIntObj(length);

  if (handle != NULL)
  {
    Tcl_IncrRefCount(handle);
    list[2] = handle;
  }
  if (list[0] == NULL || list[1] == NULL || list[2] == NULL)
    return NULL;
  return Tcl_NewListObj(blob_elems, list);
}

/*
  Construct a Tcl blob object, which has two representations:
   This handles two cases:
    -> A three element list representing a blob retrieved from the
       data store, in which case we fill in handle, if not NULL
    -> A two element list representing a locally allocated blob,
        in which case we set handle == NULL
 */

static int extract_tcl_blob(Tcl_Interp *interp, Tcl_Obj *const objv[],
                     Tcl_Obj *obj, adlb_blob_t *blob, Tcl_Obj **handle)
{
  int rc;
  Tcl_Obj **elems;
  int elem_count;
  rc = Tcl_ListObjGetElements(interp, obj, &elem_count, &elems);
  TCL_CONDITION(rc == TCL_OK && (elem_count == 2 || elem_count == 3),
                "Error interpreting %s as blob list", Tcl_GetString(obj));

  rc = Tcl_GetPtr(interp, elems[0], &blob->value);
  TCL_CHECK_MSG(rc, "Error extracting pointer from %s", Tcl_GetString(elems[0]));

  Tcl_WideInt wint;
  rc = Tcl_GetWideIntFromObj(interp, elems[1], &wint);
  // TODO: this truncates it back down to int: what is intended?
  blob->length = wint;
  TCL_CHECK_MSG(rc, "Error extracting blob length from %s",
                Tcl_GetString(elems[1]));
  if (elem_count == 2)
  {
    if (handle != NULL)
    {
      *handle = NULL;
    }
  }
  else
  {
    if (handle != NULL)
    {
      *handle = elems[2];
    }
  }
  return TCL_OK;
}

/**
 * Add blob to cache
 * blob: pointer to blob, to take ownership of
 */
static int cache_blob(Tcl_Interp *interp, int objc,
    Tcl_Obj *const objv[], adlb_datum_id id, adlb_subscript sub,
    void *blob)
{
  int rc;

  // Build key for the cache
  void *cache_key;
  size_t cache_key_len;
  bool free_cache_key;
  rc = blob_cache_key(interp, objv, &id, &sub, &cache_key,
                      &cache_key_len, &free_cache_key);
  TCL_CHECK(rc);

  // Link the blob into the cache
  bool b = table_bp_add(&blob_cache, cache_key, cache_key_len, blob);
  if (free_cache_key)
  {
    free(cache_key); 
  }
  TCL_CONDITION(b, "Error adding to blob cache");

  return TCL_OK;
}

static int uncache_blob(Tcl_Interp *interp, int objc,
    Tcl_Obj *const objv[], adlb_datum_id id, adlb_subscript sub,
    bool *found_in_cache) {
  // Build key for the cache
  void *cache_key;
  size_t cache_key_len;
  bool free_cache_key;
  int rc = blob_cache_key(interp, objv, &id, &sub,
              &cache_key, &cache_key_len, &free_cache_key);
  TCL_CHECK(rc);
  void* blob;
 
  *found_in_cache = table_bp_remove(&blob_cache, cache_key,
                                    cache_key_len, &blob);
  if (*found_in_cache)
  {
    free(blob);
  }

  if (free_cache_key)
  {
    free(cache_key);
  }
  return TCL_OK;
}

/**
   Free a local blob cached with adlb::blob_cache
   usage: adlb::blob_free <id>
 */
static int
ADLB_Blob_Free_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);

  int rc;
  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s",
                Tcl_GetString(objv[1]));

  bool found;
  DEBUG_ADLB("LOOKUP IN CACHE: {%s}\n", Tcl_GetString(objv[1]));
  rc = uncache_blob(interp, objc, objv, handle.id,
                    handle.sub.val, &found);
  TCL_CHECK(rc);
  
  if (adlb_has_sub(handle.sub.val))
  {
    TCL_CONDITION(found, "blob not cached: <%"PRId64">[%.*s]",
        handle.id, (int)handle.sub.val.length,
        (const char*)handle.sub.val.key);
  }
  else
  {
    TCL_CONDITION(found, "blob not cached: <%"PRId64">", handle.id);
  }

  rc = ADLB_PARSE_HANDLE_CLEANUP(&handle);
  TCL_CHECK(rc);

  return TCL_OK;
}

/**
   Free a local blob object.
   If the blob contains an ADLB datum id, then it should be in the
   turbine blob cache, so uncache it.
   If the blob is not associated with a datum id, then it
   was allocated with malloc, so free it
   usage: adlb::local_blob_free <struct>
 */
static int
ADLB_Local_Blob_Free_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);

  int rc;
  adlb_blob_t blob;
  
  Tcl_Obj *handle_obj;
  rc = extract_tcl_blob(interp, objv, objv[1], &blob, &handle_obj);
  TCL_CHECK(rc);

  if (handle_obj == NULL)
  {
    if (blob.value != NULL)
      free(blob.value);
    return TCL_OK;
  } else {
    //printf("uncache_blob: %s", Tcl_GetString(objv[1]));
    tcl_adlb_handle handle;
    rc = ADLB_PARSE_HANDLE(handle_obj, &handle, true);
    TCL_CHECK_MSG(rc, "Invalid handle %s",
                  Tcl_GetString(objv[1]));
    
    bool cached;
    rc = uncache_blob(interp, objc, objv, handle.id, 
                      handle.sub.val, &cached);
    TCL_CHECK(rc);

    if (!cached && blob.value != NULL)
      // Wasn't managed by cache
      free(blob.value);

    rc = ADLB_PARSE_HANDLE_CLEANUP(&handle);
    TCL_CHECK(rc);
    return TCL_OK;
  }
}

/**
   adlb::store_blob <id> <pointer> <length> [<decr>]
 */
static int
ADLB_Store_Blob_Cmd(ClientData cdata, Tcl_Interp *interp,
                    int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc == 4 || objc == 5,
                "adlb::store_blob requires 4 or 5 args!");

  int rc;
  adlb_datum_id id;
  void* pointer;
  int length;
  rc = Tcl_GetADLB_ID(interp, objv[1], &id);
  TCL_CHECK_MSG(rc, "requires id!");
  rc = Tcl_GetPtr(interp, objv[2], &pointer);
  TCL_CHECK_MSG(rc, "requires pointer!");
  rc = Tcl_GetIntFromObj(interp, objv[3], &length);
  TCL_CHECK_MSG(rc, "requires length!");

  adlb_refcounts decr = ADLB_WRITE_RC;
  if (objc == 5) {
    rc = Tcl_GetIntFromObj(interp, objv[4], &decr.write_refcount);
    TCL_CHECK_MSG(rc, "decr must be int!");
  }

  rc = ADLB_Store(id, ADLB_NO_SUB, ADLB_DATA_TYPE_BLOB, pointer, length,
                  decr, ADLB_NO_RC);
  CHECK_ADLB_STORE(rc, id);

  return TCL_OK;
}

/**
   adlb::store_blob_floats <id> [ list doubles ] [<decr>]
 */
static int
ADLB_Blob_store_floats_Cmd(ClientData cdata, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc == 3 || objc == 4, "Expected 2 or 3 args");
  int rc;
  adlb_datum_id id;
  rc = Tcl_GetADLB_ID(interp, objv[1], &id);
  TCL_CHECK_MSG(rc, "requires id!");

  int length;
  Tcl_Obj** objs;
  rc = Tcl_ListObjGetElements(interp, objv[2], &length, &objs);
  TCL_CHECK_MSG(rc, "requires list!");
  assert(length >= 0);

  TCL_CONDITION((size_t)length*sizeof(double) <= ADLB_DATA_MAX,
                "list too long!");

  for (int i = 0; i < length; i++)
  {
    double v;
    rc = Tcl_GetDoubleFromObj(interp, objs[i], &v);
    TCL_CHECK(rc);
    memcpy(xfer+(size_t)i*sizeof(double), &v, sizeof(double));
  }

  adlb_refcounts decr = ADLB_WRITE_RC;
  if (objc == 4) {
    rc = Tcl_GetIntFromObj(interp, objv[3], &decr.write_refcount);
    TCL_CHECK_MSG(rc, "decr must be int!");

  }
  rc = ADLB_Store(id, ADLB_NO_SUB, ADLB_DATA_TYPE_BLOB,
        xfer, length*(int)sizeof(double), decr, ADLB_NO_RC);
  CHECK_ADLB_STORE(rc, id);

  return TCL_OK;
}

/**
   adlb::store_blob_ints <id> [ list ints ] [<decr>]
 */
static int
ADLB_Blob_store_ints_Cmd(ClientData cdata, Tcl_Interp *interp,
                         int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc == 3 || objc == 4, "Expected 2 or 3 args");
  int rc;
  adlb_datum_id id;
  rc = Tcl_GetADLB_ID(interp, objv[1], &id);
  TCL_CHECK_MSG(rc, "requires id!");

  int length;
  Tcl_Obj** objs;
  rc = Tcl_ListObjGetElements(interp, objv[2], &length, &objs);
  TCL_CHECK_MSG(rc, "requires list!");

  TCL_CONDITION(length*(int)sizeof(int) <= ADLB_DATA_MAX,
                "list too long!");

  for (int i = 0; i < length; i++)
  {
    // TODO: should we use 64-bit ints?
    int v;
    rc = Tcl_GetIntFromObj(interp, objs[i], &v);
    TCL_CHECK(rc);
    memcpy(xfer+(size_t)i*sizeof(int), &v, sizeof(int));
  }

  adlb_refcounts decr = ADLB_WRITE_RC;
  if (objc == 4) {
    rc = Tcl_GetIntFromObj(interp, objv[3], &decr.write_refcount);
    TCL_CHECK_MSG(rc, "decr must be int!");

  }
  rc = ADLB_Store(id, ADLB_NO_SUB, ADLB_DATA_TYPE_BLOB,
        xfer, length*(int)sizeof(int), decr, ADLB_NO_RC);
  CHECK_ADLB_STORE(rc, id);

  return TCL_OK;
}

static int
ADLB_Blob_From_Int_List_Cmd(ClientData cdata, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc == 2, "Expected 1 arg");
  int rc;

  int length;
  Tcl_Obj** objs;
  rc = Tcl_ListObjGetElements(interp, objv[1], &length, &objs);
  TCL_CHECK_MSG(rc, "requires list!");
  assert(length >= 0);

  // TODO: should we use 64-bit ints?
  size_t blob_size = length * sizeof(int);
  int *blob = malloc(blob_size);
  TCL_MALLOC_CHECK(blob);

  for (int i = 0; i < length; i++)
  {
    rc = Tcl_GetIntFromObj(interp, objs[i], &blob[i]);
    TCL_CHECK(rc);
  }
  
  Tcl_Obj *result = build_tcl_blob(blob, (int)blob_size, NULL);
  TCL_MALLOC_CHECK(blob);

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

static int
ADLB_Blob_From_Float_List_Cmd(ClientData cdata, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc == 2, "Expected 1 arg");
  int rc;

  int length;
  Tcl_Obj** objs;
  rc = Tcl_ListObjGetElements(interp, objv[1], &length, &objs);
  TCL_CHECK_MSG(rc, "requires list!");
  assert(length >= 0);

  size_t blob_size = length * sizeof(double);
  double *blob = malloc(blob_size);
  TCL_MALLOC_CHECK(blob);

  for (int i = 0; i < length; i++)
  {
    rc = Tcl_GetDoubleFromObj(interp, objs[i], &blob[i]);
    TCL_CHECK(rc);
  }
  
  Tcl_Obj *result = build_tcl_blob(blob, (int)blob_size, NULL);
  TCL_MALLOC_CHECK(blob);

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   adlb::blob_from_string <string value>
 */
static int
ADLB_Blob_From_String_Cmd(ClientData cdata, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);
  int length;
  char *data = Tcl_GetStringFromObj(objv[1], &length);
  assert(length >= 0);

  TCL_CONDITION(data != NULL,
                "adlb::blob_from_string failed!");
  int length2 = length+1;

  void *blob = malloc((size_t)length2 * sizeof(char));
  memcpy(blob, data, (size_t)length2);

  Tcl_Obj* list[2];
  list[0] = Tcl_NewPtr(blob);
  list[1] = Tcl_NewIntObj(length2);
  Tcl_Obj* result = Tcl_NewListObj(2, list);

  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   adlb::blob_to_string <blob value>
   Convert null-terminated blob to string
 */
static int
ADLB_Blob_To_String_Cmd(ClientData cdata, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);
  adlb_blob_t blob;
  int rc = extract_tcl_blob(interp, objv, objv[1], &blob, NULL);
  TCL_CHECK(rc);

  TCL_CONDITION(((char*)blob.value)[blob.length-1] == '\0', "adlb::blob_to_string "
                "blob must be null terminated");
  Tcl_SetObjResult(interp, Tcl_NewStringObj(blob.value, blob.length-1));
  return TCL_OK;
}


static int
ADLB_Insert_Impl(ClientData cdata, Tcl_Interp *interp,
      int objc, Tcl_Obj *const objv[], adlb_subscript_kind sub_kind)
{
  TCL_CONDITION((objc >= 4),
                "requires at least 4 args!");
  int rc;

  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[1]));

  rc = ADLB_PARSE_SUB(objv[2], sub_kind, &handle.sub, true, true);
  TCL_CHECK_MSG(rc, "Invalid subscript argument %s",
                    Tcl_GetString(objv[2]));

  // Check for no subscript
  TCL_CONDITION(adlb_has_sub(handle.sub.val), "No subscript");

  int argpos = 3;
  Tcl_Obj *member_obj = objv[argpos++];

  adlb_data_type type;
  adlb_type_extra extra;
  rc = type_from_obj_extra(interp, objv, objv[argpos++], &type,
                           &extra);
  TCL_CHECK(rc);

  adlb_binary_data member;
  rc = tcl_obj_to_bin(interp, objv, type, extra,
                      member_obj, &xfer_buf, &member);

  // TODO: support binary subscript
  TCL_CHECK_MSG(rc, "adlb::insert <%"PRId64">[%.*s] failed, could not "
        "extract data!", handle.id, (int)handle.sub.val.length,
        (const char*)handle.sub.val.key);

  // TODO: support binary subscript
  DEBUG_ADLB("adlb::insert <%"PRId64">[\"%.*s\"]=<%s>",
               handle.id, (int)handle.sub.val.length, (const char*)handle.sub.val.key,
               Tcl_GetStringFromObj(member_obj, NULL));

  adlb_refcounts decr = ADLB_NO_RC;
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.write_refcount);
    TCL_CHECK(rc);
  }

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.read_refcount);
    TCL_CHECK(rc);
  }

  TCL_CONDITION(argpos == objc, "trailing arguments after %i not consumed",
                                argpos);


  // TODO: support accepting this as arg
  adlb_refcounts store_rc = ADLB_READ_RC;
  rc = ADLB_Store(handle.id, handle.sub.val, type,
                  member.data, member.length, decr, store_rc);

  // TODO: support binary subscript
  CHECK_ADLB_STORE_SUB(rc, handle.id, handle.sub.val);

  // Free if needed
  if (member.data != xfer_buf.data)
    ADLB_Free_binary_data(&member);
  
  ADLB_PARSE_HANDLE_CLEANUP(&handle);
  return TCL_OK;
}

/**
   usage: adlb::insert <id> <subscript> <member> <type> [<extra for type>]
                       [<write refcount decr>] [<read refcount decr>]
*/
static int
ADLB_Insert_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  return ADLB_Insert_Impl(cdata, interp, objc, objv, ADLB_SUB_CONTAINER);
}

/**
   usage: adlb::insert_struct <id> <subscript> <member> <type> [<extra for type>]
                       [<write refcount decr>] [<read refcount decr>]
*/
static int
ADLB_Insert_Struct_Cmd(ClientData cdata, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
  return ADLB_Insert_Impl(cdata, interp, objc, objv, ADLB_SUB_STRUCT);
}

/**
   usage: adlb::insert_atomic <id> <subscript>
              [<caller read refs>] [<caller write refs>]
              [<outer write decrements>] [<outer read decrements>]
   returns: 0 if the id[subscript] already existed, else 1
*/
static int
ADLB_Insert_Atomic_Cmd(ClientData cdata, Tcl_Interp *interp,
                       int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc >= 3, "Requires at least 3 args");
  int rc;
  bool b;

  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[1]));

  rc = ADLB_PARSE_SUB(objv[2], ADLB_SUB_CONTAINER, &handle.sub, true, true);
  TCL_CHECK_MSG(rc, "Invalid subscript argument %s",
                    Tcl_GetString(objv[2]));
  
  int argpos = 3;

  // Increments/decrements for outer and inner containers
  // (default no extras)
  adlb_retrieve_rc refcounts = ADLB_RETRIEVE_NO_RC;

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                    &refcounts.incr_referand.read_refcount);
    TCL_CHECK(rc);
  }
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                    &refcounts.incr_referand.write_refcount);
    TCL_CHECK(rc);
  }

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                           &refcounts.decr_self.write_refcount);
    TCL_CHECK(rc);
  }
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                           &refcounts.decr_self.read_refcount);
    TCL_CHECK(rc);
  }
  TCL_CONDITION(argpos == objc, "Trailing args starting at %i", argpos);
  

  // TODO: support binary subscript
  DEBUG_ADLB("adlb::insert_atomic: <%"PRId64">[\"%.*s\"]",
             handle.id, (int)handle.sub.val.length,
             (const char*)handle.sub.val.key);
  rc = ADLB_Insert_atomic(handle.id, handle.sub.val, refcounts, &b,
                          NULL, NULL, NULL);
  
  TCL_CONDITION(rc == ADLB_SUCCESS,
        "adlb::insert_atomic: failed: <%"PRId64">[%.*s]", handle.id,
        (int)handle.sub.val.length, (const char*)handle.sub.val.key);
  
  ADLB_PARSE_HANDLE_CLEANUP(&handle);

  Tcl_Obj* result = Tcl_NewBooleanObj(b);
  Tcl_SetObjResult(interp, result);

  return TCL_OK;
}

static int
ADLB_Lookup_Impl(Tcl_Interp *interp, int objc, Tcl_Obj *const objv[],
                 adlb_subscript_kind sub_kind, bool spin)
{
  TCL_CONDITION(objc >= 3, "adlb::lookup at least 2 arguments!");

  int rc;
  
  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[1]));

  rc = ADLB_PARSE_SUB(objv[2], sub_kind, &handle.sub, true, true);
  TCL_CHECK_MSG(rc, "Invalid subscript argument %s",
                    Tcl_GetString(objv[2]));
  // Check for no subscript
  TCL_CONDITION(adlb_has_sub(handle.sub.val), "No subscript");
 
  // TODO: support binary subscript
  DEBUG_ADLB("adlb::lookup <%"PRId64">[\"%.*s\"]", handle.id,
      (int)handle.sub.val.length, (const char*)handle.sub.val.key);

  int argpos = 3;
  adlb_data_type type;
  int len;

  // Optional reference decrement argument, defaults to 0
  adlb_retrieve_rc refcounts = ADLB_RETRIEVE_NO_RC;
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                &refcounts.decr_self.read_refcount);
    TCL_CHECK(rc);
  }

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                &refcounts.incr_referand.read_refcount);
    TCL_CHECK(rc);
  }

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                &refcounts.decr_self.write_refcount);
    TCL_CHECK(rc);
  }

  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++],
                &refcounts.incr_referand.write_refcount);
    TCL_CHECK(rc);
  }

  TCL_CONDITION(argpos == objc,
          "extra trailing arguments starting at argument %i", argpos);

  do {
    // TODO: support binary subscript
    rc = ADLB_Retrieve(handle.id, handle.sub.val, refcounts, &type,
                      xfer, &len);
    if (rc != ADLB_SUCCESS) // Check outside loop
      break;
  } while (spin && rc == ADLB_SUCCESS && len < 0);
  
  TCL_CONDITION(rc == ADLB_SUCCESS, "lookup failed for: <%"PRId64">[%.*s]",
                  handle.id, (int)handle.sub.val.length,
                  (const char*)handle.sub.val.key);

  // TODO: support binary subscript
  TCL_CONDITION(len >= 0, "adlb::lookup <%"PRId64">[\"%.*s\"] not found",
                handle.id, (int)handle.sub.val.length,
                (const char*)handle.sub.val.key);
  assert(type != ADLB_DATA_TYPE_NULL);

  Tcl_Obj* result = NULL;
  rc = adlb_data_to_tcl_obj(interp, objv, handle.id, type,
                    ADLB_TYPE_EXTRA_NULL, xfer, len, &result);
  TCL_CHECK(rc);

  DEBUG_ADLB("adlb::lookup <%"PRId64">[\"%.*s\"]=<%s>", handle.id,
        (int)handle.sub.val.length, (const char*)handle.sub.val.key,
        Tcl_GetStringFromObj(result, NULL));
  
  ADLB_PARSE_HANDLE_CLEANUP(&handle);
  
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   Lookup something in an ADLB container

   usage: adlb::lookup <id> <subscript>
        [<decr readers>] [<incr readers referand>]
        [<decr writers>] [<incr writers referand>]
   decr (readers|writers): decrement reference counts.  Default is zero.
   incr (readers|writers) referand: increment reference counts of referand
   returns the member
*/
static int
ADLB_Lookup_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
    return ADLB_Lookup_Impl(interp, objc, objv, ADLB_SUB_CONTAINER, false);
}

/**
  Lookup something in an ADLB struct
   usage: adlb::lookup_struct <id> <subscript>
        [<decr readers>] [<incr readers referand>]
        [<decr writers>] [<incr writers referand>]
   subscript: integer, or list of integers for struct indices
   decr (readers|writers): decrement reference counts.  Default is zero.
   incr (readers|writers) referand: increment reference counts of referand
   returns the member
*/
static int
ADLB_Lookup_Struct_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
    return ADLB_Lookup_Impl(interp, objc, objv, ADLB_SUB_STRUCT, false);
}

/**
   usage: adlb::lookup_spin <id> <subscript> [<decr readers>] [<decr writers>]
        [<incr readers referand>] [<incr writers referand>]
   decr (readers|writers): decrement reference counts.  Default is zero.
   incr (readers|writers) referand: increment reference counts of referand
   returns the member
*/
static int
ADLB_Lookup_Spin_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
    return ADLB_Lookup_Impl(interp, objc, objv, ADLB_SUB_CONTAINER, true);
}

/**
   usage: adlb::lock <id> => false (try again) or
                             true (locked by caller)
*/
static int
ADLB_Lock_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);

  int rc;
  adlb_datum_id id;
  rc = Tcl_GetADLB_ID(interp, objv[1], &id);
  TCL_CHECK_MSG(rc, "argument must be a long integer!");

  bool locked;
  rc = ADLB_Lock(id, &locked);
  TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64"> failed!", id);

  Tcl_Obj* result = Tcl_NewBooleanObj(locked);
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   usage: adlb::unlock <id>
*/
static int
ADLB_Unlock_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);

  int rc;
  adlb_datum_id id;
  rc = Tcl_GetADLB_ID(interp, objv[1], &id);
  TCL_CHECK_MSG(rc, "argument must be a long integer!");

  rc = ADLB_Unlock(id);
  TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64"> failed!", id);

  return TCL_OK;
}

/**
   usage: adlb::unique => id
*/
static int
ADLB_Unique_Cmd(ClientData cdata, Tcl_Interp *interp,
                int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(1);

  adlb_datum_id id;
  int rc = ADLB_Unique(&id);
  ASSERT(rc == ADLB_SUCCESS);

  // DEBUG_ADLB("adlb::unique: <%"PRId64">", id);

  Tcl_Obj* result = Tcl_NewADLB_ID(id);
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   usage: adlb::container_typeof <id>
*/
static int
ADLB_Typeof_Cmd(ClientData cdata, Tcl_Interp *interp,
		int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);
  int rc;
  adlb_datum_id id;
  rc = Tcl_GetADLB_ID(interp, objv[1], &id);
  TCL_CHECK(rc);

  adlb_data_type type;
  rc = ADLB_Typeof(id, &type);
  TCL_CONDITION(rc == ADLB_SUCCESS,
                "adlb::container_typeof <%"PRId64"> failed!", id);

  // DEBUG_ADLB("adlb::container_typeof: <%"PRId64"> is: %i\n", id, type);

  const char *type_string = ADLB_Data_type_tostring(type);

  // DEBUG_ADLB("adlb::container_typeof: <%"PRId64"> is: %s",
  //            id, type_string);

  Tcl_Obj* result = Tcl_NewStringObj(type_string, -1);
  Tcl_SetObjResult(interp, result);

  return TCL_OK;
}

/**
   usage: adlb::container_typeof <id>
*/
static int
ADLB_Container_Typeof_Cmd(ClientData cdata, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(2);

  adlb_datum_id id;
  int rc;

  rc = Tcl_GetADLB_ID(interp, objv[1], &id);
  TCL_CHECK(rc);

  adlb_data_type key_type, val_type;
  rc = ADLB_Container_typeof(id, &key_type, &val_type);
  TCL_CONDITION(rc == ADLB_SUCCESS,
                "adlb::container_typeof <%"PRId64"> failed!", id);

  const char *key_type_string = ADLB_Data_type_tostring(val_type);
  const char *val_type_string = ADLB_Data_type_tostring(key_type);

  Tcl_Obj* types[2] = {Tcl_NewStringObj(key_type_string, -1),
                       Tcl_NewStringObj(val_type_string, -1)};

  Tcl_Obj* result = Tcl_NewListObj(2, types);
  Tcl_SetObjResult(interp, result);

  return TCL_OK;
}

static int
ADLB_Reference_Impl(ClientData cdata, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[],
                             adlb_subscript_kind sub_kind);

/**
   usage: adlb::container_reference
      <container_id> <subscript> <reference> <reference_type>

      reference_type is type of container field
      e.g. ref for plain turbine IDs
*/
static int
ADLB_Container_Reference_Cmd(ClientData cdata, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
  return ADLB_Reference_Impl(cdata, interp, objc, objv,
                             ADLB_SUB_CONTAINER);
}

/**
   usage: adlb::struct_reference
      <struct_id> <subscript> <reference> <reference_type>
      subscript is a list of indices into struct
      reference_type is type of container field
      e.g. ref for plain turbine IDs
*/
static int
ADLB_Struct_Reference_Cmd(ClientData cdata, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
  return ADLB_Reference_Impl(cdata, interp, objc, objv,
                             ADLB_SUB_STRUCT);
}

// container_reference, supporting different subscript formats
static int
ADLB_Reference_Impl(ClientData cdata, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[],
                             adlb_subscript_kind sub_kind)
{
  TCL_ARGS(5);

  int rc;
  tcl_adlb_handle handle;
  rc = ADLB_PARSE_HANDLE(objv[1], &handle, true);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[1]));

  rc = ADLB_PARSE_SUB(objv[2], sub_kind, &handle.sub, true, true);
  TCL_CHECK_MSG(rc, "Invalid subscript %s", Tcl_GetString(objv[2]));
  // Check for no subscript
  TCL_CONDITION(adlb_has_sub(handle.sub.val), "Invalid subscript argument");

  tcl_adlb_handle ref_handle;
  rc = ADLB_PARSE_HANDLE(objv[3], &ref_handle, false);
  TCL_CHECK_MSG(rc, "Invalid handle %s", Tcl_GetString(objv[3]));

  adlb_data_type ref_type;
  adlb_type_extra extra;
  // ignores extra type info
  rc = type_from_obj_extra(interp, objv, objv[4], &ref_type,
                           &extra);
  TCL_CHECK(rc);

  // TODO: optionally take num of read/write references to transfer
  adlb_refcounts transfer_rc = ADLB_READ_RC;

  // DEBUG_ADLB("adlb::container_reference: <%"PRId64">[%s] => <%"PRId64">\n",
  //            id, subscript, reference);
  rc = ADLB_Container_reference(handle.id, handle.sub.val,
              ref_handle.id, ref_handle.sub.val, ref_type, transfer_rc);
  
  ADLB_PARSE_HANDLE_CLEANUP(&handle);
  ADLB_PARSE_HANDLE_CLEANUP(&ref_handle);

  TCL_CONDITION(rc == ADLB_SUCCESS, "<%"PRId64"> failed!", handle.id);
  return TCL_OK;
}

/**
   usage: adlb::container_size <container_id> [ <read decr> ] [ <write decr> ]
*/
static int
ADLB_Container_Size_Cmd(ClientData cdata, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc >= 2, "must have at least 2 arguments");

  int argpos = 1;
  adlb_datum_id container_id;
  int rc;
  rc = Tcl_GetADLB_ID(interp, objv[argpos++], &container_id);
  TCL_CHECK_MSG(rc, "argument is not a valid ID!");

  adlb_refcounts decr = ADLB_NO_RC;
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.read_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }
  if (argpos < objc)
  {
    rc = Tcl_GetIntFromObj(interp, objv[argpos++], &decr.write_refcount);
    TCL_CHECK_MSG(rc, "Expected integer argument");
  }

  TCL_CONDITION(argpos == objc, "unexpected trailing args at %ith arg", argpos);

  int size;
  // DEBUG_ADLB("adlb::container_size: <%"PRId64">",
  //            container_id, size);
  rc = ADLB_Container_size(container_id, &size, decr);
  TCL_CONDITION(rc == ADLB_SUCCESS,
                "adlb::container_size: <%"PRId64"> failed!",
                container_id);
  Tcl_Obj* result = Tcl_NewIntObj(size);
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
   usage: adlb::write_refcount_incr <id> [ increment ]
*/
static int
ADLB_Write_Refcount_Incr_Cmd(ClientData cdata, Tcl_Interp *interp,
                     int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION((objc == 2 || objc == 3),
                "requires 1 or 2 args!");
  int rc;
  adlb_datum_id container_id;
  rc = ADLB_EXTRACT_HANDLE_ID(objv[1], &container_id);
  TCL_CHECK(rc);

  adlb_refcounts incr = ADLB_WRITE_RC;
  if (objc == 3)
  {
    rc = Tcl_GetIntFromObj(interp, objv[2], &incr.write_refcount);
    TCL_CHECK_MSG(rc, "Error extracting reference count");
  }

  // DEBUG_ADLB("adlb::write_refcount_incr: <%"PRId64">", container_id);
  rc = ADLB_Refcount_incr(container_id, incr);

  if (rc != ADLB_SUCCESS)
    return TCL_ERROR;
  return TCL_OK;
}

/**
   usage: adlb::write_refcount_decr <id> <decrement>
*/
static int
ADLB_Write_Refcount_Decr_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION((objc == 2 || objc == 3),
                "requires 1 or 2 args!");
  int rc;
  adlb_datum_id container_id;
  rc = Tcl_GetADLB_ID(interp, objv[1], &container_id);
  TCL_CHECK(rc);

  int decr_w = 1;
  if (objc == 3)
  {
    rc = Tcl_GetIntFromObj(interp, objv[2], &decr_w);
    TCL_CHECK_MSG(rc, "Error extracting reference count");
  }

  // DEBUG_ADLB("adlb::write_refcount_decr: <%"PRId64">", container_id);
  adlb_refcounts decr = { .read_refcount = 0, .write_refcount = -decr_w };
  rc = ADLB_Refcount_incr(container_id, decr);

  if (rc != ADLB_SUCCESS)
    return TCL_ERROR;
  return TCL_OK;
}

/*
  Implement multiple reference count commands.
  amount: if null, assume 1
  bool: negate the reference count
 */
static int
ADLB_Refcount_Incr_Impl(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[],
                   adlb_refcount_type type,
                   Tcl_Obj *var, Tcl_Obj *amount,
                   bool negate)
{
  int rc;

  adlb_datum_id id;
  rc = ADLB_EXTRACT_HANDLE_ID(var, &id);
  TCL_CHECK(rc);

  int change = 1; // Default
  if (amount != NULL)
  {
    rc = Tcl_GetIntFromObj(interp, amount, &change);
    TCL_CHECK(rc);
  }

  if (negate)
  {
    change = -change;
  }

 // DEBUG_ADLB("adlb::refcount_incr: <%"PRId64">", id);

  adlb_refcounts incr = ADLB_NO_RC;
  if (type == ADLB_READ_REFCOUNT || type == ADLB_READWRITE_REFCOUNT)
  {
    incr.read_refcount = change;
  }
  if (type == ADLB_WRITE_REFCOUNT || type == ADLB_READWRITE_REFCOUNT)
  {
    incr.write_refcount = change;
  }
  rc = ADLB_Refcount_incr(id, incr);

  if (rc != ADLB_SUCCESS)
    return TCL_ERROR;
  return TCL_OK;
}

static int refcount_mode(Tcl_Interp *interp, Tcl_Obj *const objv[],
                          Tcl_Obj* obj, adlb_refcount_type *mode)
{
  const char *mode_string = Tcl_GetString(obj);
  TCL_CONDITION(mode_string != NULL, "invalid refcountmode argument");
  if (strcmp(mode_string, "r") == 0)
  {
    *mode = ADLB_READ_REFCOUNT;
    return TCL_OK;
  }
  else if (strcmp(mode_string, "w") == 0)
  {
    *mode = ADLB_WRITE_REFCOUNT;
    return TCL_OK;
  }
  else if (strcmp(mode_string, "rw") == 0)
  {
    *mode = ADLB_READWRITE_REFCOUNT;
    return TCL_OK;
  }
  else
  {
    char err[strlen(mode_string) + 20];
    sprintf(err, "unknown refcount mode %s!", mode_string);
    Tcl_AddErrorInfo(interp, err);
    return TCL_ERROR;
  }
}

/**
   usage: adlb::refcount_incr <container_id> <refcount_type> <change>
   refcount_type in { r, w, rw }
*/
static int
ADLB_Refcount_Incr_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION((objc == 4), "requires 4 args!");

  adlb_refcount_type mode;
  int rc = refcount_mode(interp, objv, objv[2], &mode);
  TCL_CHECK(rc);
  return ADLB_Refcount_Incr_Impl(cdata, interp, objc, objv, mode,
                          objv[1], objv[3], false);
}

static int
ADLB_Read_Refcount_Incr_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION((objc == 2 || objc == 3), "requires 2-3 args!");
  Tcl_Obj *amount = (objc == 3) ? objv[2] : NULL;

  return ADLB_Refcount_Incr_Impl(cdata, interp, objc, objv,
              ADLB_READ_REFCOUNT, objv[1], amount, false);
}

static int
ADLB_Read_Refcount_Decr_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION((objc == 2 || objc == 3), "requires 2-3 args!");
  Tcl_Obj *amount = (objc == 3) ? objv[2] : NULL;

  return ADLB_Refcount_Incr_Impl(cdata, interp, objc, objv,
              ADLB_READ_REFCOUNT, objv[1], amount, true);
}


/**
   usage: adlb::read_refcount_enable
   If not set, all read reference count operations are ignored
 **/
static int
ADLB_Enable_Read_Refcount_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  adlb_code rc = ADLB_Read_refcount_enable();
  TCL_CONDITION(rc == ADLB_SUCCESS, "Unexpected failure");
  return TCL_OK;
}

/**
  Usage: adlb::xpt_init <filename> <flush policy> <max index val size>
  filename: the filename of the checkpoint file.  If empty string, checkpointing to file
            not initialized
  flush policy: no_flush, periodic_flush, or always_flush
  max index val size: maximum size of value to store in index
 */
static int
ADLB_Xpt_Init_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_ARGS(4);

#ifdef ENABLE_XPT
  const char *filename = Tcl_GetString(objv[1]);
  if (strlen(filename) == 0) {
    filename = NULL; // ADLB interface takes null instead of empty string
  }
  const char *flush_policy_s = Tcl_GetString(objv[2]);
  adlb_xpt_flush_policy flush_policy;
  if (strcmp(flush_policy_s, "no_flush"))
  {
    flush_policy = ADLB_NO_FLUSH;
  }
  else if (strcmp(flush_policy_s, "periodic_flush"))
  {
    flush_policy = ADLB_PERIODIC_FLUSH;
  }
  else if (strcmp(flush_policy_s, "always_flush"))
  {

    flush_policy = ADLB_ALWAYS_FLUSH;
  }
  else
  {
    TCL_RETURN_ERROR("Invalid flush policy: %s", flush_policy_s);
  }

  int max_index_val;
  int rc = Tcl_GetIntFromObj(interp, objv[3], &max_index_val);
  TCL_CHECK(rc);

  adlb_code ac = ADLB_Xpt_init(filename, flush_policy, max_index_val);
  TCL_CONDITION(ac == ADLB_SUCCESS,
                "Error while initializing checkpointing");
  return TCL_OK;
#else
  TCL_RETURN_ERROR("Checkpointing not enabled");
  return TCL_ERROR;
#endif
}

/**
  Usage: adlb::xpt_finalize
 */
static int
ADLB_Xpt_Finalize_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
#ifdef ENABLE_XPT
  TCL_ARGS(1);
  adlb_code ac = ADLB_Xpt_finalize();
  TCL_CONDITION(ac == ADLB_SUCCESS, "Error while finalizing checkpointing");
  return TCL_OK;
#else
  TCL_RETURN_ERROR("Checkpointing not enabled");
  return TCL_ERROR;
#endif
}

/**
  usage: adlb::xpt_write <key blob> <val blob> <persist mode> <index add>
  persist mode: no_persist, persist, or persist_flush: whether/how to
                persist to file
  index add: int interpreted as boolean: whether to add to index
 */
static int
ADLB_Xpt_Write_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
#ifdef ENABLE_XPT
  TCL_ARGS(5);
  int rc;
  adlb_code ac;

  adlb_blob_t key_blob, val_blob;
  rc = extract_tcl_blob(interp, objv, objv[1], &key_blob, NULL);
  TCL_CHECK(rc);
  
  rc = extract_tcl_blob(interp, objv, objv[2], &val_blob, NULL);
  TCL_CHECK(rc);

  adlb_xpt_persist persist_mode;
  const char *persist_mode_s = Tcl_GetString(objv[3]);
  if (strcmp(persist_mode_s, "no_persist") == 0)
  {
    persist_mode = ADLB_NO_PERSIST;
  }
  else if (strcmp(persist_mode_s, "persist") == 0)
  {
    persist_mode = ADLB_PERSIST;
  }
  else if (strcmp(persist_mode_s, "persist_flush") == 0)
  {
    persist_mode = ADLB_PERSIST_FLUSH;
  }
  else
  {
    TCL_RETURN_ERROR("Invalid persist mode: %s", persist_mode_s);
  }

  int index_add_i;
  rc = Tcl_GetBooleanFromObj(interp, objv[4], &index_add_i);
  TCL_CHECK(rc);
  bool index_add = (index_add_i != 0);
 
  ac = ADLB_Xpt_write(key_blob.value, key_blob.length, val_blob.value,
                      val_blob.length, persist_mode, index_add);
  TCL_CONDITION(ac == ADLB_SUCCESS, "Error writing checkpoint");
  return TCL_OK;
#else
  TCL_RETURN_ERROR("Checkpointing not enabled");
  return TCL_ERROR;
#endif
}

/**
  usage: adlb::xpt_lookup <checkpoint key> [ <checkpoint value> ]
  return value: bool indicating whether checkpoint exists
  checkpoint value: name of variable for packed value of checkpoint
                    as blob
  checkpoint key: packed checkpoint key as blob
 */
static int
ADLB_Xpt_Lookup_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
#ifdef ENABLE_XPT
  TCL_CONDITION(objc == 2 || objc == 3, "Must provide 1 or 2 arguments" );
  int rc;
  adlb_code ac;

  adlb_blob_t key;
  rc = extract_tcl_blob(interp, objv, objv[1], &key, NULL);
  TCL_CHECK(rc);
 
  adlb_binary_data val;
  ac = ADLB_Xpt_lookup(key.value, key.length, &val);
  TCL_CONDITION(ac == ADLB_SUCCESS || ac == ADLB_NOTHING,
                "Error looking up checkpoint");
  bool found = (ac == ADLB_SUCCESS);
 
  bool outArgProvided = (objc > 2);

  if (found && outArgProvided)
  {
    // put into Tcl blob and put in variable caller requested
    ADLB_Own_data(NULL, &val); // Make sure we own memory
    Tcl_Obj *tclVal = build_tcl_blob(val.caller_data, val.length,
                                     NULL);
    TCL_CONDITION(tclVal != NULL, "Error building blob");
    tclVal = Tcl_ObjSetVar2(interp, objv[2], NULL,
                           tclVal, EMPTY_FLAG);
    TCL_CONDITION(tclVal != NULL, "Error setting output argument %s",
                  Tcl_GetString(objv[2]));

  }
  else if (found)
  {
    // Not returning, so free memory
    ADLB_Free_binary_data(&val);
  }

  Tcl_SetObjResult(interp, Tcl_NewIntObj(found));
  return TCL_OK;
#else
  TCL_RETURN_ERROR("Checkpointing not enabled");
  return TCL_ERROR;
#endif
}


/*
   Pack a TCL container value represented as a TCL dict or array.
   Handles nesting
   Consturct compound type from Tcl arguments .
   argpos: updated to consume multiple type names from command line
 */
static int
get_compound_type(Tcl_Interp *interp, int objc, Tcl_Obj *const objv[],
                int *argpos, compound_type *types)
{
  int rc;

  /* slurp up relevant data types: get all nested containers plus the
   * value type.
   */
  size_t types_size = 16;
  int len = 0;
  adlb_data_type *type_arr = malloc(sizeof(adlb_data_type) * types_size);
  TCL_CONDITION(type_arr != NULL, "Error allocating memory");

  adlb_type_extra *extras = malloc(sizeof(adlb_type_extra) * types_size);
  TCL_CONDITION_GOTO(extras != NULL, exit_err, "Error allocating memory");
  int to_consume = 1; // Min additional number that must be consumed

  // Must consume at least the outermost type
  while (to_consume > 0) {
    TCL_CONDITION_GOTO(*argpos < objc, exit_err,
                       "Consumed past end of arguments");
    
    if (types_size <= len)
    {
      types_size *= 2;
      type_arr = realloc(type_arr, sizeof(adlb_data_type) * types_size);
      TCL_CONDITION_GOTO(type_arr != NULL, exit_err,
                        "Error allocating memory");
      
      extras = realloc(extras, sizeof(adlb_type_extra) * types_size);
      TCL_CONDITION_GOTO(extras != NULL, exit_err,
                        "Error allocating memory");
    }

    adlb_data_type curr;
    adlb_type_extra extra;
    rc = type_from_obj_extra(interp, objv, objv[*argpos], &curr,
                             &extra);
    TCL_CHECK_GOTO(rc, exit_err);
    
    type_arr[len] = curr;
   
    if (extra.valid)
    {
      extras[len] = extra;
    }
    else
    {
      extras[len] = ADLB_TYPE_EXTRA_NULL;
    }

    // Make sure we consume more types
    switch (curr)
    {
      case ADLB_DATA_TYPE_CONTAINER:
        assert(to_consume == 1);
        to_consume = 2; // Key and val
        break;
      case ADLB_DATA_TYPE_MULTISET:
        assert(to_consume == 1);
        to_consume = 1; // Val
        break;
      default:
        to_consume--;
        break;
    }

    len++;
    (*argpos)++;
  }

  types->types = type_arr;
  types->extras = extras;
  types->len = len;
  return TCL_OK;
exit_err:
  if (type_arr != NULL)
  {
    free(type_arr);
  }
  if (extras != NULL)
  {
    free(extras);
  }
  return TCL_ERROR;
}

/**
  usage: adlb::xpt_pack (<type> <value>)*
 */
static int
ADLB_Xpt_Pack_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  int rc;
  adlb_data_code dc;

  adlb_buffer packed;
  int pos = 0;
  bool using_caller_buf;
  // Caller blob needs to own data, so don't provide a static buffer
  dc = ADLB_Init_buf(NULL, &packed, &using_caller_buf, 2048);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error initializing buffer");

  int argpos = 1;
  int field = 0;
  while (argpos < objc)
  {
    // We might need to pack compound types
    compound_type compound_type;
    rc = get_compound_type(interp, objc, objv, &argpos,
                                 &compound_type);
    TCL_CHECK(rc);

    TCL_CONDITION(argpos < objc,
                  "Last argument missing value");
    Tcl_Obj *val = objv[argpos++];

    DEBUG_ADLB("Packing entry #%i type %s @ byte %i", field,
                  ADLB_Data_type_tostring(compound_type.types[0]), pos);
   
    // pack incrementally into buffer
    int ctype_pos = 0;
    rc = tcl_obj_bin_append(interp, objv, compound_type, ctype_pos,
            val, true, &packed, &using_caller_buf, &pos);
    TCL_CHECK(rc);

    free_compound_type(&compound_type);
    field++;
  }

  Tcl_Obj *packedBlob = build_tcl_blob(packed.data, pos, NULL);
  Tcl_SetObjResult(interp, packedBlob);
  return TCL_OK;
}

/**
  usage: adlb::xpt_unpack (<var name>)* <packed data> (<var type>)
  packed data: tcl blob format
  var type: ADLB types for the packed fields
  The number of var names and types must match
 */
static int
ADLB_Xpt_Unpack_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
  TCL_CONDITION(objc >= 2, "Must have at least 1 arg");
  TCL_CONDITION(objc % 2 == 0, "Must have paired var names and types");
  int rc;
  int fieldCount = (objc - 2) / 2;
  
  adlb_blob_t packed;
  rc = extract_tcl_blob(interp, objv, objv[fieldCount + 1], &packed, NULL);
  TCL_CHECK(rc);

  int packed_pos = 0;

  for (int field = 0; field < fieldCount; field++)
  {
    Tcl_Obj *varName = objv[field + 1];
    Tcl_Obj *typeO = objv[field + fieldCount + 2];

    // Get type of object
    adlb_data_type type;
    adlb_type_extra extra;
    rc = type_from_obj_extra(interp, objv, typeO, &type, &extra);
    TCL_CHECK(rc);

    // Unpack next entry from buffer
    const void *entry;
    int entry_length;
    adlb_data_code dc = ADLB_Unpack_buffer(type, packed.value, packed.length,
          &packed_pos, &entry, &entry_length);
    TCL_CONDITION(dc != ADLB_DATA_DONE, "Hit end of buffer after unpacking "
               "%i/%i fields", field, fieldCount); 
    TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error unpacking field %i "
            "from buffer", field);
    
    DEBUG_ADLB("Unpacking entry #%i type %s @ byte %i from blob %p "
                "[%i bytes] entry: offset %li [%i bytes]", field,
                ADLB_Data_type_tostring(type), packed_pos, packed.value,
                packed.length, entry - packed.value, entry_length);

    Tcl_Obj *obj;
    rc = adlb_data_to_tcl_obj(interp, objv, ADLB_DATA_ID_NULL,
          type, extra, entry, entry_length, &obj);
    TCL_CHECK(rc);
    
    // Store result into location caller requested
    obj = Tcl_ObjSetVar2(interp, varName, NULL, obj, EMPTY_FLAG);
    TCL_CONDITION(obj != NULL, "error setting field %s",
                  Tcl_GetString(varName));
  }
  return TCL_OK;
}

/**
  usage: adlb::xpt_reload <checkpoint file name>
  returns: statistics about reload.  Dict with "ranks" containing total
            number of ranks.  An entry is added for each rank that was
            reloaded (no entry present if not loaded by this process).
            Each rank entry is a dict with statistics: valid and
            invalid counts.
 */
static int
ADLB_Xpt_Reload_Cmd(ClientData cdata, Tcl_Interp *interp,
                   int objc, Tcl_Obj *const objv[])
{
#ifdef ENABLE_XPT
  TCL_ARGS(2);
  const char *filename = Tcl_GetString(objv[1]);

  adlb_code ac;
  adlb_xpt_load_stats stats;
  ac = ADLB_Xpt_reload(filename, &stats);
  TCL_CONDITION(ac == ADLB_SUCCESS, "Error reloading checkpoint from file %s",
                filename);

  Tcl_Obj *stat_dict = Tcl_NewDictObj();

  Tcl_DictObjPut(interp, stat_dict, Tcl_NewStringObj("ranks", -1),
                                     Tcl_NewWideIntObj(stats.ranks));

  Tcl_Obj *valid_key = Tcl_NewStringObj("valid", -1);
  Tcl_Obj *invalid_key = Tcl_NewStringObj("invalid", -1);
  for (int i = 0; i < stats.ranks; i++)
  {
    adlb_xpt_load_rank_stats *rstats = &stats.rank_stats[i];
    if (rstats->loaded)
    {
      Tcl_Obj *rank_dict = Tcl_NewDictObj();
      Tcl_DictObjPut(interp, stat_dict, Tcl_NewIntObj(i), rank_dict);
      Tcl_DictObjPut(interp, rank_dict, valid_key,
                     Tcl_NewIntObj(rstats->valid));
      Tcl_DictObjPut(interp, rank_dict, invalid_key,
                     Tcl_NewIntObj(rstats->invalid));
    }
  }

  free(stats.rank_stats);
  
  Tcl_SetObjResult(interp, stat_dict);
  return TCL_OK;
#else
  TCL_RETURN_ERROR("Checkpointing not enabled");
  return TCL_ERROR;
#endif
}

/**
   Same as builtin dict create except don't allow duplicates.
   usage: adlb::dict_create key1 val1 key2 val2 ...
 */
static int
ADLB_Dict_Create_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  Tcl_Obj *dict = Tcl_NewDictObj();

  TCL_CONDITION(objc % 2 == 1, "Must have even number of args "
      "(matching keys and values): got odd number %i", objc - 1);

  int rc;

  for (int i = 1; i < objc; i += 2)
  {
    Tcl_Obj *key = objv[i];
    Tcl_Obj *val = objv[i + 1];
    if (i != 1)
    {
      // CHeck for duplicates
      Tcl_Obj *old_val;
      rc = Tcl_DictObjGet(interp, dict, key, &old_val);
      TCL_CHECK(rc);

      TCL_CONDITION(old_val == NULL, "Tried to create dictionary with "
            "duplicate values \"%s\" and \"%s\" for key \"%s\"",
            Tcl_GetString(old_val), Tcl_GetString(val), Tcl_GetString(key));
    }
      
    rc = Tcl_DictObjPut(interp, dict, key, val);
    TCL_CHECK(rc);
  }
  Tcl_SetObjResult(interp, dict);
  return TCL_OK;
}

/**
 * Handle input of forms:
 * - 124 (plain ID) => 124 & no subscript
 * - 1234.123.424.53 (id + struct indices - . separated)
 *    => id=1234 subscript="123.424.53" (not counting null terminator)
 */
int
ADLB_Extract_Handle(Tcl_Interp *interp, Tcl_Obj *const objv[],
        Tcl_Obj *obj, adlb_datum_id *id, const char **subscript,
        int *subscript_len)
{
  int rc;
  // Leave interp NULL so we don't get error message there
  rc = Tcl_GetADLB_ID(NULL, obj, id);
  if (rc == TCL_OK)
  {
    *subscript = NULL;
    *subscript_len = 0;
    return TCL_OK;
  }
      
  int str_handle_len;
  const char *str_handle = Tcl_GetStringFromObj(obj, &str_handle_len);
  TCL_CONDITION(str_handle != NULL, "Error getting string handle");

  // Separate ID from remainder of subscript
  const char *sep = memchr(str_handle, '.', (size_t)str_handle_len);
  TCL_CONDITION(sep != NULL, "Invalid ADLB handle %s", str_handle);

  int prefix_len = (int)(sep - str_handle);

  adlb_data_code dc;
  dc = ADLB_Int64_parse(str_handle, (size_t)prefix_len, id);
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Expected first element "
        "in handle to be valid ADLB ID: %s", str_handle);

  // Return subscript
  *subscript = (const char *) sep + 1; // Move past '.'
  // String length of remainder
  *subscript_len = str_handle_len - prefix_len - 1;
  
  return TCL_OK;
}

int
ADLB_Extract_Handle_ID(Tcl_Interp *interp, Tcl_Obj *const objv[],
        Tcl_Obj *obj, adlb_datum_id *id)
{
  const char *subscript;
  int subscript_len;
  return ADLB_Extract_Handle(interp, objv, obj, id, &subscript,
                             &subscript_len);
}


int
ADLB_Parse_Subscript(Tcl_Interp *interp, Tcl_Obj *const objv[],
  Tcl_Obj *obj, adlb_subscript_kind sub_kind, tcl_adlb_sub_parse *parse,
  bool append, bool use_scratch)
{
  int rc;
  if (sub_kind == ADLB_SUB_CONTAINER)
  {
    if (!append || parse->val.length == 0)
    {
      rc = Tcl_GetADLB_Subscript(obj, &parse->val);
      TCL_CHECK(rc);
      parse->buf.data = NULL;
      parse->buf.length = 0;
    }
    else
    {
      adlb_subscript tmp_sub;
      rc = Tcl_GetADLB_Subscript(obj, &tmp_sub);
      TCL_CHECK(rc);

      rc = append_subscript(interp, objv, &parse->val, tmp_sub,
                            &parse->buf);
      TCL_CHECK(rc);
    }
  }
  else
  {
    assert(sub_kind == ADLB_SUB_STRUCT);
    int subscript_len;
    char *subscript = Tcl_GetStringFromObj(obj, &subscript_len);
    TCL_CONDITION(subscript != NULL, "Could not extract string for "
                  "subscript");
    if (subscript_len == 0)
    {
      if (!append)
      {
        parse->val = ADLB_NO_SUB;
        // Ensure buffer initialized
        parse->buf.data = NULL;
        parse->buf.length = 0;
      }
    }
    else
    {
      if (!append)
      {
        // Initialize buffer
        if (use_scratch)
        {
          parse->buf = tcl_adlb_scratch_buf;
        }
        else
        {
          parse->buf.data = NULL;
          parse->buf.length = 0;
        }
      }

      bool using_scratch = (parse->buf.data == tcl_adlb_scratch);

      rc = PARSE_STRUCT_SUB(subscript, subscript_len,
                          &parse->buf, &parse->val, &using_scratch, append);
      TCL_CHECK(rc);
    }
  }
  return TCL_OK;
}

int
ADLB_Parse_Subscript_Cleanup(Tcl_Interp *interp, Tcl_Obj *const objv[],
                             tcl_adlb_sub_parse *parse)
{
  // If we're using tcl_adlb_scratch, free it
  free_non_scratch(parse->buf);
  return TCL_OK;
}


/**
 * Append a subscript to an existing one
 * Assume that buf is either malloced buffer, or the
 * scratch buffer
 */
static int append_subscript(Tcl_Interp *interp,
      Tcl_Obj *const objv[], adlb_subscript *sub, adlb_subscript to_append,
      adlb_buffer *buf)
{
  bool using_scratch = (buf->data == tcl_adlb_scratch);

  // resize buffer to fit new and old subscript
  adlb_data_code dc = ADLB_Resize_buf(buf, &using_scratch,
                          (int)(sub->length + to_append.length));
  TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error resizing");

  if (sub->length > 0)
  {
    if (buf->data != sub->key)
    {
      // if not in buffer, copy old subscript to buffer
      memcpy(buf->data, sub->key, sub->length);
    }
    // overwrite null terminator with '.'
    buf->data[sub->length - 1] = '.';
  }

  // append the new subscript
  memcpy(&buf->data[sub->length], to_append.key, to_append.length);

  sub->key = buf->data;
  sub->length += to_append.length;
  return TCL_OK;
}

/**
 * Parse a Tcl ADLB subscript into a binary ADLB subscript
 * str: string containing Tcl subscript
 * length: remaining length of string
 * adlb_subscript_kind: kind of leading subscript (might be prefix of
 *                      different subscript)
 * buf: buffer to use/return data.  Should be initialized by caller,
 *      optionally with storage that can be used. Initial size
 *      indicates size of buffer given by caller.
 *      Upon return, pointer will be updated if memory allocated in here.
 * TODO: this currently works for some array subscripts too.. 
 * TODO: but it breaks for e.g. general string subscripts
 * using_caller_buf: if true, storage is owned by caller and shouldn't be
 *                   freed
 * append: if true, append to existing subscript
 */
static int ADLB_Parse_Struct_Subscript(Tcl_Interp *interp,
  Tcl_Obj *const objv[],
  const char *str, int length, adlb_buffer *buf, adlb_subscript *sub,
  bool *using_caller_buf, bool append)
{
  assert(length >= 0);

  adlb_data_code dc;
  /*
   * Let's assume struct subscript, which is a '.'-separated list of
   * integer indices, for now, since this is main use case.
   * ADLB representation is '.'-separated list of text integers,
   * null-terminated.  Since we currently use almost the same
   * representation, just copy it over and ensure it's null terminated.
   * We'll leave validation for the ADLB server
   */

  if (append && sub->length > 0)
  {
    dc = ADLB_Resize_buf(buf, using_caller_buf,
              (int)sub->length + length + 1);
    TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error expanding buf");
   
    if (buf->data != sub->key)
    {
      memcpy(buf->data, sub->key, sub->length);
    }

    buf->data[sub->length-1] = '.'; // Replace null terminator

    memcpy(&buf->data[sub->length], str, (size_t)length);
    buf->data[length] = '\0';

    sub->length += (size_t)length + 1; // Length includes terminator;
    sub->key = buf->data;
  }
  else
  {
    dc = ADLB_Resize_buf(buf, using_caller_buf, length + 1);
    TCL_CONDITION(dc == ADLB_DATA_SUCCESS, "Error expanding buf");

    memcpy(buf->data, str, (size_t)length);
    buf->data[length] = '\0';

    sub->length = (size_t)length + 1; // Length includes terminator;
    sub->key = buf->data;
  }

  return TCL_OK;
}

int
ADLB_Parse_Handle(Tcl_Interp *interp, Tcl_Obj *const objv[],
        Tcl_Obj *obj, tcl_adlb_handle *parse, bool use_scratch)
{
  int rc;
  const char *subscript;
  int subscript_len;
  rc = ADLB_EXTRACT_HANDLE(obj, &parse->id, &subscript, &subscript_len);
  TCL_CHECK(rc);

  if (subscript == NULL)
  {
    parse->sub.val = ADLB_NO_SUB;
    // Ensure buffer initialized
    parse->sub.buf.data = NULL;
    parse->sub.buf.length = 0;
  }
  else
  {
    if (use_scratch)
    {
      parse->sub.buf = tcl_adlb_scratch_buf;
    }
    else
    {
      parse->sub.buf.data = NULL;
      parse->sub.buf.length = 0;
    }

    // TODO: container subscripts?

    bool using_scratch = use_scratch;
    rc = PARSE_STRUCT_SUB(subscript, subscript_len,
                        &parse->sub.buf, &parse->sub.val,
                        &using_scratch, false);
    TCL_CHECK(rc);
  }

  return TCL_OK;
}

int
ADLB_Parse_Handle_Cleanup(Tcl_Interp *interp, Tcl_Obj *const objv[],
                          tcl_adlb_handle *parse)
{
  // If we're using tcl_adlb_scratch, free it
  free_non_scratch(parse->sub.buf);
  return TCL_OK;
}

static int
ADLB_Subscript_Impl(ClientData cdata, Tcl_Interp *interp,
     int objc, Tcl_Obj *const objv[], adlb_subscript_kind sub_kind)
{
  TCL_CONDITION(objc >= 2, "Must have at least one argument");

  int rc;
  int old_handle_len;
  char *old_handle = Tcl_GetStringFromObj(objv[1], &old_handle_len);
  assert(old_handle != NULL);

  int subscripts = objc - 2;

  if (sub_kind == ADLB_SUB_CONTAINER)
  {
    TCL_CONDITION(subscripts <= 1, "Only support one level of"
                                   "subscripting for container");
  }
  else
  {
    // Only support two kinds
    assert(sub_kind == ADLB_SUB_STRUCT);
  }

  int new_handle_len = old_handle_len;
  for (int i = 0; i < subscripts; i++)
  {
    int sub_len;
    char *sub = Tcl_GetStringFromObj(objv[i + 2], &sub_len);
    assert(sub != NULL);
    new_handle_len += sub_len + 1;  // subscript plus "." separator
  }
  
  Tcl_Obj *result = Tcl_NewObj();
  TCL_MALLOC_CHECK(result);

  rc = Tcl_AttemptSetObjLength(result, new_handle_len);
  // TCL_AttemptSetObjLength doesn't use standard Tcl return codes
  TCL_CONDITION(rc == 1, "Error setting object length");

  // Copy in subscripts to object
  char *result_ptr = result->bytes;
  assert(result_ptr != NULL);
  memcpy(result_ptr, old_handle, (size_t)old_handle_len);
  result_ptr += old_handle_len;

  for (int i = 0; i < subscripts; i++)
  {
    int sub_len;
    char *sub = Tcl_GetStringFromObj(objv[i + 2], &sub_len);
    assert(sub != NULL);

    // subscript plus "." separator
    *result_ptr = '.';
    result_ptr++;

    memcpy(result_ptr, sub, (size_t)sub_len);
    result_ptr += sub_len;
  }
  
  Tcl_SetObjResult(interp, result);
  return TCL_OK;
}

/**
  Build a handle for an id + subscript into a struct.

  adlb::subscript_struct <handle> [<subscript>]*
  handle: either an id, or a handle built by this function
  subscript: a valid subscript into a struct
 */
static int
ADLB_Subscript_Struct_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  return ADLB_Subscript_Impl(cdata, interp, objc, objv, ADLB_SUB_STRUCT);
}

static int
ADLB_Subscript_Container_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  return ADLB_Subscript_Impl(cdata, interp, objc, objv, ADLB_SUB_CONTAINER);
}

/**
   usage: adlb::fail
 */
static int
ADLB_Fail_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  ADLB_Fail(1);
  return TCL_OK;
}

/**
   usage: adlb::abort
 */
static int
ADLB_Abort_Cmd(ClientData cdata, Tcl_Interp *interp,
               int objc, Tcl_Obj *const objv[])
{
  ADLB_Abort(1);
  return TCL_OK;
}


/**
   usage: adlb::finalize <b>
   If b, finalize MPI
 */
static int
ADLB_Finalize_Cmd(ClientData cdata, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const objv[])
{
  int rc;

  // Finalize field objs before ADLB struct type stuff cleared up
  rc = field_name_objs_finalize(interp, objv);
  TCL_CHECK(rc);

  rc = ADLB_Finalize();
  if (rc != ADLB_SUCCESS)
    printf("WARNING: ADLB_Finalize() failed!\n");
  TCL_ARGS(2);
  int b;
  Tcl_GetBooleanFromObj(interp, objv[1], &b);

  if (must_comm_free)
    MPI_Comm_free(&adlb_comm);

  if (b)
    MPI_Finalize();
  turbine_debug_finalize();

  rc = blob_cache_finalize();
  TCL_CHECK(rc);

  return TCL_OK;
}

static void blob_free_callback(const void *key, size_t key_len,
                               void *blob)
{
  free(blob);
}

static int blob_cache_finalize(void)
{
  // Free table structure and any contained blobs
  table_bp_free_callback(&blob_cache, false, blob_free_callback);
  return TCL_OK;
}

/**
   Shorten object creation lines.  "adlb::" namespace is prepended
 */
#define ADLB_NAMESPACE "adlb::"
#define COMMAND(tcl_function, c_function) \
    Tcl_CreateObjCommand(interp, ADLB_NAMESPACE tcl_function, c_function, \
                         NULL, NULL);

/**
   Called when Tcl loads this extension
 */
int DLLEXPORT
Tcladlb_Init(Tcl_Interp* interp)
{
  if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL)
    return TCL_ERROR;

  if (Tcl_PkgProvide(interp, "ADLB", "0.1") == TCL_ERROR)
    return TCL_ERROR;

  tcl_adlb_init(interp);

  return TCL_OK;
}

void
tcl_adlb_init(Tcl_Interp* interp)
{
  COMMAND("init",      ADLB_Init_Cmd);
  COMMAND("declare_struct_type", ADLB_Declare_Struct_Type_Cmd);
  COMMAND("server",    ADLB_Server_Cmd);
  COMMAND("rank",      ADLB_Rank_Cmd);
  COMMAND("amserver",  ADLB_AmServer_Cmd);
  COMMAND("size",      ADLB_Size_Cmd);
  COMMAND("servers",   ADLB_Servers_Cmd);
  COMMAND("workers",   ADLB_Workers_Cmd);
  COMMAND("barrier",   ADLB_Barrier_Cmd);
  COMMAND("hostmap_lookup",   ADLB_Hostmap_Lookup_Cmd);
  COMMAND("hostmap_list",     ADLB_Hostmap_List_Cmd);
  COMMAND("get_priority",   ADLB_Get_Priority_Cmd);
  COMMAND("reset_priority", ADLB_Reset_Priority_Cmd);
  COMMAND("set_priority",   ADLB_Set_Priority_Cmd);
  COMMAND("put",       ADLB_Put_Cmd);
  COMMAND("spawn",     ADLB_Spawn_Cmd);
  COMMAND("get",       ADLB_Get_Cmd);
  COMMAND("iget",      ADLB_Iget_Cmd);
  COMMAND("create",    ADLB_Create_Cmd);
  COMMAND("multicreate",ADLB_Multicreate_Cmd);
  COMMAND("exists",    ADLB_Exists_Cmd);
  COMMAND("exists_sub", ADLB_Exists_Sub_Cmd);
  COMMAND("closed", ADLB_Closed_Cmd);
  COMMAND("store",     ADLB_Store_Cmd);
  COMMAND("retrieve",  ADLB_Retrieve_Cmd);
  COMMAND("retrieve_decr",  ADLB_Retrieve_Decr_Cmd);
  COMMAND("acquire_ref",  ADLB_Acquire_Ref_Cmd);
  COMMAND("acquire_write_ref",  ADLB_Acquire_Write_Ref_Cmd);
  COMMAND("acquire_sub_ref",  ADLB_Acquire_Sub_Ref_Cmd);
  COMMAND("acquire_sub_write_ref",  ADLB_Acquire_Sub_Write_Ref_Cmd);
  COMMAND("enumerate", ADLB_Enumerate_Cmd);
  COMMAND("retrieve_blob", ADLB_Retrieve_Blob_Cmd);
  COMMAND("retrieve_decr_blob", ADLB_Retrieve_Blob_Decr_Cmd);
  COMMAND("blob_free",  ADLB_Blob_Free_Cmd);
  COMMAND("local_blob_free",  ADLB_Local_Blob_Free_Cmd);
  COMMAND("store_blob", ADLB_Store_Blob_Cmd);
  COMMAND("store_blob_floats", ADLB_Blob_store_floats_Cmd);
  COMMAND("store_blob_ints", ADLB_Blob_store_ints_Cmd);
  COMMAND("blob_from_float_list", ADLB_Blob_From_Float_List_Cmd);
  COMMAND("blob_from_int_list", ADLB_Blob_From_Int_List_Cmd);
  COMMAND("blob_from_string", ADLB_Blob_From_String_Cmd);
  COMMAND("blob_to_string", ADLB_Blob_To_String_Cmd);
  COMMAND("enable_read_refcount",  ADLB_Enable_Read_Refcount_Cmd);
  COMMAND("refcount_incr", ADLB_Refcount_Incr_Cmd);
  COMMAND("read_refcount_incr", ADLB_Read_Refcount_Incr_Cmd);
  COMMAND("read_refcount_decr", ADLB_Read_Refcount_Decr_Cmd);
  COMMAND("write_refcount_incr", ADLB_Write_Refcount_Incr_Cmd);
  COMMAND("write_refcount_decr", ADLB_Write_Refcount_Decr_Cmd);
  COMMAND("insert",    ADLB_Insert_Cmd);
  COMMAND("insert_struct",    ADLB_Insert_Struct_Cmd);
  COMMAND("insert_atomic", ADLB_Insert_Atomic_Cmd);
  COMMAND("lookup",    ADLB_Lookup_Cmd);
  COMMAND("lookup_struct",    ADLB_Lookup_Struct_Cmd);
  COMMAND("lookup_spin", ADLB_Lookup_Spin_Cmd);
  COMMAND("lock",      ADLB_Lock_Cmd);
  COMMAND("unlock",    ADLB_Unlock_Cmd);
  COMMAND("unique",    ADLB_Unique_Cmd);
  COMMAND("typeof",    ADLB_Typeof_Cmd);
  COMMAND("container_typeof",    ADLB_Container_Typeof_Cmd);
  COMMAND("container_reference", ADLB_Container_Reference_Cmd);
  COMMAND("container_size",      ADLB_Container_Size_Cmd);
  COMMAND("struct_reference", ADLB_Struct_Reference_Cmd);
  COMMAND("xpt_init", ADLB_Xpt_Init_Cmd);
  COMMAND("xpt_finalize", ADLB_Xpt_Finalize_Cmd);
  COMMAND("xpt_write", ADLB_Xpt_Write_Cmd);
  COMMAND("xpt_lookup", ADLB_Xpt_Lookup_Cmd);
  COMMAND("xpt_pack", ADLB_Xpt_Pack_Cmd);
  COMMAND("xpt_unpack", ADLB_Xpt_Unpack_Cmd);
  COMMAND("xpt_reload", ADLB_Xpt_Reload_Cmd);
  COMMAND("dict_create", ADLB_Dict_Create_Cmd);
  COMMAND("subscript_struct", ADLB_Subscript_Struct_Cmd);
  COMMAND("subscript_container", ADLB_Subscript_Container_Cmd);
  COMMAND("fail",      ADLB_Fail_Cmd);
  COMMAND("abort",     ADLB_Abort_Cmd);
  COMMAND("finalize",  ADLB_Finalize_Cmd);

  // Export all commands
  Tcl_Namespace *ns = Tcl_FindNamespace(interp,
          ADLB_NAMESPACE, NULL, TCL_GLOBAL_ONLY);
  Tcl_Export(interp, ns, "*", true);
}
