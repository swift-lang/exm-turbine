# Copyright 2014 University of Chicago and Argonne National Laboratory
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

# Test Coaster executor - basic sanity test

package require turbine 0.5.0

set COASTER_WORK_TYPE 1

proc coaster_task { x i } {
  turbine::c::coaster_run "echo" [ list Hello World ] \
          "coaster_task_success $x $i" "coaster_task_fail"
}

proc coaster_task_success { x i } {
  turbine::store_integer $x $i
}

proc coaster_task_fail { } {

}

proc main {} {
  global COASTER_WORK_TYPE

  for { set i 0 } { $i < 100 } { incr i } {
    # Add a task to the noop executor
    turbine::allocate x integer
    turbine::rule "" "coaster_task $x $i" type $COASTER_WORK_TYPE

    turbine::rule [ list $x ] "puts \"COASTER task output set: $i\"; \
                               turbine::read_refcount_decr $x"
  }
}

turbine::defaults
turbine::init $servers Turbine [ list $turbine::COASTER_EXEC_NAME ]
turbine::enable_read_refcount

set coaster_work_type [ turbine::adlb_work_type $turbine::COASTER_EXEC_NAME ]

set service_url $env(COASTER_SERVICE_URL)
set settings $env(COASTER_SETTINGS)

turbine::c::coaster_register $coaster_work_type $service_url $settings
turbine::start main 
turbine::finalize

puts OK

# Help Tcl free memory
proc exit args {}
