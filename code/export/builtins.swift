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

// Swift/Turbine builtins defined here

// HACK: have preprocessor ignore this: now automatically included
#define BUILTINS_SWIFT

#ifndef BUILTINS_SWIFT
#define BUILTINS_SWIFT

// External type
type external void;

@pure
(void o) make_void () "turbine" "0.0.2" "make_void";

// Arithmetic
@pure @minmax @builtin_op=MAX_INT
(int o) max_integer     (int i1, int i2) "turbine" "0.0.2" "max_integer";
@pure @minmax @builtin_op=MIN_INT
(int o) min_integer     (int i1, int i2) "turbine" "0.0.2" "min_integer";
@pure @minmax @builtin_op=POW_INT
(float o) pow_integer     (int i1, int i2) "turbine" "0.0.2" "pow_integer";

// This is used by the string+ concatenation operator
@pure @builtin_op=STRCAT
(string o) strcat(string... args) "turbine" "0.0.2" "strcat";

// This is used by the string/ directory catenation operator
@pure @builtin_op=DIRCAT
(string o) dircat(string... args) "turbine" "0.0.2" "dircat";

@pure @commutative @builtin_op=XOR
(boolean o) xor (boolean i1, boolean i2) "turbine" "0.0.2" "neq_integer";

@pure @minmax @builtin_op=MAX_FLOAT
(float o) max_float     (float i1, float i2) "turbine" "0.0.2" "max_float";
@pure @minmax @builtin_op=MIN_FLOAT
(float o) min_float     (float i1, float i2) "turbine" "0.0.2" "min_float";
@pure @builtin_op=POW_FLOAT
(float o) pow_float     (float i1, float i2) "turbine" "0.0.2" "pow_float";

// Type conversion
@pure @builtin_op=INTTOSTR
(string o) fromint(int i)  "turbine" "0.0.2" "fromint";
@pure @builtin_op=STRTOINT
(int o)    toint(string i) "turbine" "0.0.2" "toint";
@pure @builtin_op=FLOATTOSTR
(string o) fromfloat(float i)  "turbine" "0.0.2" "fromfloat";
@pure @builtin_op=STRTOFLOAT
(float o) tofloat(string i)  "turbine" "0.0.2" "tofloat";
@pure @builtin_op=INTTOFLOAT
(float o) itof    (int i) "turbine"  "0.0.2" "itof";
// Do not optimize this- it is for synchronization tests
(int o) zero(void v) "turbine" "0.0.4" "zero";

// I/O
(void o) trace (int|float|string|boolean... args) "turbine" "0.0.2" "trace"
    [ "turbine::trace_impl <<args>>" ];
(void o) sleep_trace (float secs, int|float|string|boolean... args) "turbine" "0.0.2"
                                                            "sleep_trace";

// Container operations
@pure @implements=range
(int res[]) range(int start, int end) "turbine" "0.0.2" "range"
  [ "turbine::range_work <<res>> <<start>> <<end>> 1" ];
@pure @implements=range_step
(int res[]) rangestep(int start, int end, int step) "turbine" "0.0.2" "rangestep"
  [ "turbine::range_work <<res>> <<start>> <<end>> <<step>>" ];
@pure
<T> (int n) size(T A[]) "turbine" "0.0.5" "container_size"
  [ "set <<n>> [ turbine::container_size_local <<A>> ]" ];
@pure
<K, V> (boolean o) contains(V A[K], K key) "turbine" "0.0.5" "contains"
  [ "set <<o>> [ adlb::exists_sub <<A>> <<key>> ]" ];

// Updateable variables
(updateable_float o) init_updateable(float i) "turbine" "0.0.2" "init_updateable";

// Information about cluster
// pure because it won't change during program execution
@pure
(int n) adlb_servers() "turbine" "0.0.2" "adlb_servers_future"
    [ "set <<n>> [ turbine::adlb_servers ]" ];
@pure
(int n) turbine_engines() "turbine" "0.0.2" "turbine_engines_future"
    [ "set <<n>> [ turbine::turbine_engines ]" ];
@pure
(int n) turbine_workers() "turbine" "0.0.2" "turbine_workers_future"
    [ "set <<n>> [ turbine::turbine_workers ]" ];

// Basic file ops

// filename has a special implementation hardcoded in compiler
@pure @stc_intrinsic=FILENAME
(string n) filename(file x) "turbine" "0.0.2" "";

@pure @implements=input_file
(file f) input(string filename) "turbine" "0.0.2" "input_file" [
  "set <<f>> [ turbine::input_file_local <<filename>> ]"
];

@pure @implements=input_file
(file f) input_file(string filename) "turbine" "0.0.2" "input_file" [
  "set <<f>> [ turbine::input_file_local <<filename>> ]"
];

(url f) input_url(string url) "turbine" "0.0.2" "input_url";

// Substitute for C ternary operator:
@pure
(int o) ternary(boolean b, int i1, int i2) "turbine" "0.0.2"
[ "if { <<b>> } { set <<o>> <<i1>> } else { set <<o>> <<i2>> }" ];

#endif
