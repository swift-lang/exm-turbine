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
# Turbine APP.TCL

# Functions for launching external apps

namespace eval turbine {
  namespace export unpack_args exec_external poll_mock async_exec_coasters
  # Run external appplication
  # cmd: executable to run
  # kwopts: keyword options.  Valid are:
  #         stdout=file stderr=file
  # args: command line args as strings
  proc exec_external { cmd kwopts args } {

    setup_redirects $kwopts stdin_src stdout_dst stderr_dst
    log "shell: $cmd $args $stdin_src $stdout_dst $stderr_dst"

    # TODO: remove this - STC will call a coasters-specific launch function
    #       directly
    if {[string match "coaster*" $cmd]} {
      set cmd [string triml $cmd "coaster"]
      set loop_obj [launch_coaster $cmd $stdin_src $stdout_dst $stderr_dst {*}$args]
    } else {
      set start [ clock milliseconds ]
      exec $cmd {*}$args $stdin_src $stdout_dst $stderr_dst
      set stop [ clock milliseconds ]
      set duration [ format "%0.3f" [ expr ($stop-$start)/1000.0 ] ]
      log "shell command duration: $duration"
    }
  }

  # Set specified vars in outer scope for stdin, stdout and stderr
  # based on parameters present in provided dictionary
  proc setup_redirects { kwopts stdin_var stdout_var stderr_var } {
    #FIXME: strange behaviour can happen if user args have e.g "<"
    # or ">" or "|" at start
    upvar 1 $stdin_var stdin_src
    upvar 1 $stdout_var stdout_dst
    upvar 1 $stderr_var stderr_dst

    # Default to sending stdout/stderr to process stdout/stderr
    set stdin_src "<@stdin"
    set stdout_dst ">@stdout"
    set stderr_dst "2>@stderr"

    if { [ dict exists $kwopts stdin ] } {;
      set stdin_src "<[ dict get $kwopts stdin ]"
    }
    if { [ dict exists $kwopts stdout ] } {
      set dst [ dict get $kwopts stdout ]
      ensure_directory_exists2 $dst
      set stdout_dst ">$dst"
    }
    if { [ dict exists $kwopts stderr ] } {
      set dst [ dict get $kwopts stderr ]
      ensure_directory_exists2 $dst
      set stderr_dst "2>$dst"
    }
  }

  # Launch a coasters job that will execute asynchronously
  # cmd: command to run
  # outfiles: list of output files
  # TODO: also list input files?
  # cmdargs: arguments for command
  # kwopts: options, including input/output redirects and other settings
  # continuation: optionally, a code fragment to run after finishing the
  #         task.  TODO: may want to assume that this is a function call
  #         so we can concatenate any output arguments to string?
  #proc async_exec_coasters { outfiles cmds kwopts {continuation {}}} { }
  #The control flow goes like this ->
  # STC generated TCL code -> async_exec_coasters -> launch_coaster
  proc async_exec_coasters {  cmd outfiles cmdargs kwopts {continuation {}} } {
    setup_redirects $kwopts stdin_src stdout_dst stderr_dst
    # Check to see if we were passed continuation
    set has_continuation [ expr [ string length $continuation ] > 0 ]

    log "cmd     : $cmd "
    log "args    : $cmdargs"
    log "outfile : $outfiles"
    log "kwopts  : $kwopts"
    log "continuation : $continuation $has_continuation"

    set loop_obj [launch_coaster $cmd $stdin_src $stdout_dst $stderr_dst $continuation $cmdargs]
  }

  proc poll_mock { } {
    set status_string(0) UNSUBMITTED
    set status_string(1) SUBMITTED
    set status_string(2) ACTIVE
    set status_string(3) SUSPENDED
    set status_string(4) RESUMED
    set status_string(5) FAILED
    set status_string(6) CANCELLED
    set status_string(7) COMPLETED
    set status_string(8) SUBMITTING
    set status_string(16) STAGE_IN
    set status_string(17) STAGE_OUT
    set status_string(9999) UNKNOWN

    global g_job_info
    global g_job_count
    set slots 0
    foreach {id info} $g_job_info {
      dict with info {
        set status [CoasterSWIGGetJobStatus $client_ptr $job_ptr]
        #puts "MOCK_POLL : JOB_ID( $id ) : $status_string($status)"
        if {$status == 7} {
          cleanup_coaster $loop_ptr $client_ptr $job_ptr
          puts "MOCK_POLL : Job Succeeded, Removing from list"
          puts "MOCK_POLL : Continuation : $continuation "
          eval $continuation
          dict unset g_job_info $id
        } elseif {$status == 5} {
          cleanup_coaster $loop_ptr $client_ptr $job_ptr
          puts "MOCK_POLL : Job failed, Removing from list"
          dict unset g_job_info $id
        } else {
          incr slots
        }
      }
    }
    puts "Slots: $slots"
    return $slots
  }


  proc poll_job_status { loop_ptr client_ptr job } {
    set rcode1 [CoasterSWIGGetJobStatus $cl'2ient_ptr $job1]
  }

  proc cleanup_coaster { loop_ptr client_ptr job1 } {
    set rcode [CoasterSWIGClientDestroy $client_ptr]
    set rcode [CoasterSWIGLoopDestroy $loop_ptr]
  }

  #Issue #503
  proc launch_coaster { cmd stdin_src stdout_dst stderr_dst continuation args} {
    log "launch_coaster: cmd  : $cmd"
    log "launch_coaster: args : $args"

    set stdout_dst [string trim $stdout_dst <>]
    if { $stdout_dst == "@stdout" } {
      log "launch_coaster : stdout not defined, setting to empty"
      set stdout_dst ""
    }
    log "launch_coaster: stdout_dst : $stdout_dst"

    set stderr_dst [string trim $stderr_dst 2>]
    if { $stderr_dst == "2>@stderr" } {
      log "launch_coaster : stdout not defined, setting to empty"
      set stderr_dst ""
    }
    log "launch_coaster: stderr_dst : $stderr_dst"

    package require coaster 0.0
    # TODO : Handle these env variables not being set
    set coaster_service_url $::env(COASTER_SERVICE_URL)
    set coaster_settings    $::env(COASTER_SETTINGS)
    set coaster_jobmanager  $::env(COASTER_JOBMANAGER)
    log "launch_coaster: COASTER_SERVICE_URL = $coaster_service_url"
    log "launch_coaster: COASTER_SETTINGS    = $coaster_settings"
    log "launch_coaster: COASTER_JOBMANAGER  = $coaster_jobmanager"

    set loop_ptr [CoasterSWIGLoopCreate]
    set client_ptr [CoasterSWIGClientCreate $loop_ptr $coaster_service_url]
    set x [CoasterSWIGClientSettings $client_ptr $coaster_settings]
    log "launch_coaster: Error code from CoasterSWIGClientSettings $x"

    # Job stuff
    # TODO: The second parameter is the jobmanager.
    # "local?"  use the workers already connected to the coaster service.
    # "fork" fork tasks as processes on the node coaster service runs
    # Other jobmanagers not defined.
    set job1 [CoasterSWIGJobCreate $cmd $coaster_jobmanager]
    #set job1 [CoasterSWIGJobCreate $cmd "fork"]

    #CoasterSWIGJobSettings job_obj dir args attributes env_vars stdout_loc stderr_loc"
    log "launch_coaster : CoasterSWIGJobSettings $job1 \"\" $args \"\" \"\" $stdout_dst $stderr_dst "
    set rcode [CoasterSWIGJobSettings $job1 "" $args "" "" $stdout_dst $stderr_dst]

    set rcode [CoasterSWIGSubmitJob $client_ptr $job1]
    log "launch_coaster: Job1 submitted"

    # Inset the new job pointers to the global dict
    # TODO : make this a proc
    global g_job_info
    global g_job_count
    set info_exists [info exists g_job_count ]
    if {$info_exists == 0} {
      set g_job_count 0
      puts "Initialised!"
    }
    dict set g_job_info $g_job_count loop_ptr $loop_ptr
    dict set g_job_info $g_job_count client_ptr $client_ptr
    dict set g_job_info $g_job_count job_ptr $job1
    dict set g_job_info $g_job_count continuation $continuation
    incr g_job_count
    log "Current job count : $g_job_count"
    return job1
  }

  #Issue #501
  # [TODO] This proc is deprecated, and replaced by launch_coaster
  proc exec_coaster { cmd stdin_src stdout_dst stderr_dst args} {
    log "exec_coaster: cmd : $cmd"
    log "exec_coaster: args : $args"

    set stdout_dst [string trim $stdout_dst <>]
    if { $stdout_dst == "@stdout" } {
      log "exec_coaster : stdout not defined, setting to empty"
      set stdout_dst ""
    }
    log "exec_coaster: stdout_dst : $stdout_dst"

    set stderr_dst [string trim $stderr_dst 2>]
    if { $stderr_dst == "2>@stderr" } {
      log "exec_coaster : stdout not defined, setting to empty"
      set stderr_dst ""
    }
    log "exec_coaster: stderr_dst : $stderr_dst"

    package require coaster 0.0

    set loop_ptr [CoasterSWIGLoopCreate]
    set client_ptr [CoasterSWIGClientCreate $loop_ptr 127.0.0.1:53001]
    set coaster_settings $::env(COASTER_SETTINGS)
    log "COASTER_SETTINGS = [$coaster_settings]"
    set x [CoasterSWIGClientSettings $client_ptr $coaster_settings]
    log "exec_coaster: Error code from CoasterSWIGClientSettings $x"

    # Job stuff
    set job1 [CoasterSWIGJobCreate $cmd]

    #CoasterSWIGJobSettings job_obj dir args attributes env_vars stdout_loc stderr_loc"
    log "exec_coaster : CoasterSWIGJobSettings $job1 \"\" $args \"\" \"\" $stdout_dst $stderr_dst "
    set rcode [CoasterSWIGJobSettings $job1 "" $args "" "" $stdout_dst $stderr_dst]

    set rcode [CoasterSWIGSubmitJob $client_ptr $job1]
    log "exec_coaster: Job1 submitted"

    log "exec_coaster: Waiting for Job1"
    set rcode [CoasterSWIGWaitForJob $client_ptr $job1]
    log "exec_coaster: Job1 complete"

    set rcode [CoasterSWIGClientDestroy $client_ptr]

    set rcode [CoasterSWIGLoopDestroy $loop_ptr]
  }

  # Alternative implementation
  proc ensure_directory_exists2 { f } {
    set dirname [ file dirname $f ]
    if { $dirname == "." } {
      return
    }
    # recursively create
    file mkdir $dirname
  }

  # For file f = "/d1/d2/f", ensure /d1/d2 exists
  proc ensure_directory_exists { f } {
      log "ensure_directory_exists: $f"
      set c [ string range $f 0 0 ]
      set A [ file split $f ]
      debug "path components: $A"
      set d [ lreplace $A end end ]
      set p [ join $d "/" ]
      if { [ string equal [ string range $p 0 1 ] "//" ] } {
          # This was an absolute path
          set p [ string replace $p 0 1 "/" ]
      }
      log "checking directory: $p"
      if { ! [ file isdirectory $p ] } {
          log "making directory: $p"
          file mkdir $p
      }
  }

  # Unpack arguments from closed container of any nesting into flat list
  # Container must be deep closed (i.e. all contents closed)
  proc unpack_args { container nest_level is_file } {
    set res [ list ]
    unpack_args_rec $container $nest_level $is_file res
    return $res
  }

  proc unpack_args_rec { container nest_level is_file res_var } {
    upvar 1 $res_var res

    if { $nest_level == 0 } {
      error "Nest_level < 1: $nest_level"
    }

    if { $nest_level == 1 } {
      # 1d array
      unpack_unnested_container $container $is_file res
    } else {
      # Iterate in key order
      set contents [ adlb::enumerate $container dict all 0 ]
      set sorted_keys [ lsort -integer [ dict keys $contents ] ]
      foreach key $sorted_keys {
        set inner [ dict get $contents $key ]
        unpack_args_rec $inner [ expr {$nest_level - 1} ] $is_file res
      }
    }
  }

  proc unpack_unnested_container { container is_file res_var } {
    upvar 1 $res_var res

    # Iterate in key order
    set contents [ adlb::enumerate $container dict all 0 ]
    set sorted_keys [ lsort -integer [ dict keys $contents ] ]
    foreach key $sorted_keys {
      set member [ dict get $contents $key ]
      if { $is_file } {
        lappend res [ retrieve_string [ get_file_path $member ] ]
      } else {
        lappend res [ retrieve $member ]
      }
    }
  }

}
