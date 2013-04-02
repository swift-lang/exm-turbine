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

# Turbine builtin functions

# All builtins will have signature:
#   f <OUTPUT LIST> <INPUT LIST>
# where the lists are Tcl lists of TDs
# even if some of the arguments are not used
# The uniformity allows the STC code generator to simply write all
# calls to builtins the same way

namespace eval turbine {

    # User functions
    namespace export enumerate literal shell call_composite

    # Debugging/performance-testing functions
    namespace export set1

    # Bring in Turbine extension features
    namespace import c::new c::typeof
    namespace import c::insert c::log

    # User function
    # This name conflicts with a Tcl built-in - it cannot be exported
    proc trace { signal inputs } {
        rule $inputs "turbine::trace_body \"$signal\" $inputs" \
            name "trace"
    }

    proc trace_body { signal args } {
        set valuelist [ list ]
        foreach v $args {
            set value [ retrieve_decr $v ]
            lappend valuelist $value
        }
        trace_impl2 $valuelist
        if { $signal != "" } {
            store_void $signal
        }
    }
    proc trace_impl { args } {
        # variadic version
        trace_impl2 $args
    }

    proc trace_impl2 { arglist } {
        set n [ llength $arglist ]
        puts -nonewline "trace: "
        set first 1
        foreach value $arglist {
            if { $first } {
              set first 0
            } else {
              puts -nonewline ","
            }
            puts -nonewline $value
        }
        puts ""
    }

    # # For tests/debugging
    proc sleep_trace { signal inputs } {
      # parent stack and output arguments not read
      if { ! [ string length $inputs ] } {
        error "trace: received no arguments!"
      }
      set secs [ lindex $inputs 0 ]
      set args [ lreplace $inputs 0 0]
      rule "sleep_trace" $inputs $turbine::WORK $adlb::RANK_ANY 1 \
           "turbine::sleep_trace_body $signal $secs $args"
    }
    proc sleep_trace_body { signal secs inputs } {
      set secs_val [ retrieve_decr_float $secs ]
      after [ expr {round($secs_val * 1000)} ]
      puts "AFTER"
      trace_body $signal $inputs
    }

    # User function
    proc range { result inputs } {
        # Assume that there was a container slot opened
        # that can be owned by range (this works with stc's calling
        #   conventions which don't close assigned arrays)
        set start [ lindex $inputs 0 ]
        set end [ lindex $inputs 1 ]
        rule [ list $start $end ] "range_body $result $start $end" \
              type $turbine::CONTROL name "range-$result" 
    }

    proc range_body { result start end } {

        set start_value [ retrieve_decr_integer $start ]
        set end_value   [ retrieve_decr_integer $end ]

        range_work $result $start_value $end_value 1
    }

    proc rangestep { result inputs } {
        # Assume that there was a container slot opened
        # that can be owned by range
        set start [ lindex $inputs 0 ]
        set end [ lindex $inputs 1 ]
        set step [ lindex $inputs 2 ]
        rule "rangestep-$result" [ list $start $end $step ] \
            $turbine::CONTROL $adlb::RANK_ANY 1\
            "rangestep_body $result $start $end $step"
    }

    proc rangestep_body { result start end step } {

        set start_value [ retrieve_decr_integer $start ]
        set end_value   [ retrieve_decr_integer $end ]
        set step_value   [ retrieve_decr_integer $step ]

        range_work $result $start_value $end_value $step_value
    }

    proc range_work { result start end step } {
        if { $start <= $end } {
            set k 0
            set slot_drop 0
            for { set i $start } { $i <= $end } { incr i $step } {
                allocate td integer
                store_integer $td $i

                if { [ expr {$i + $step > $end} ] } {
                  # Drop on last iter
                  set slot_drop 1
                }
                container_insert $result $k $td $slot_drop
                incr k
            }
        } else {
            # no contents, but have to close
            adlb::slot_drop $result
        }
    }

    # User function
    # Construct a distributed container of sequential integers
    proc drange { result start end parts } {
        rule "$start $end" "drange_body $result $start $end $parts" \
            type $turbine::CONTROL name "drange-$result"
    }

    proc drange_body { result start end parts } {

        set start_value [ retrieve_decr $start ]
        set end_value   [ retrieve_decr $end ]
        set parts_value [ retrieve_decr $parts ]
        set size        [ expr {$end_value - $start_value + 1} ]
        set step        [ expr {$size / $parts_value} ]

        global WORK_TYPE
        for { set i 0 } { $i < $parts_value } { incr i } {
            # top-level container
            allocate_container c integer
            container_insert $result $i $c
            # start
            set s [ expr {$i *  $step} ]
            # end
            set e [ expr {$s + $step - 1} ]

            set prio [ get_priority ]
            adlb::put $adlb::RANK_ANY $WORK_TYPE(CONTROL) \
                "command priority: $prio range_work $c $s $e 1" \
                $prio 1
        }
        # close container
        adlb::slot_drop $result
    }

    # User function
    # Loop over a distributed container
    proc dloop { loop_body stack container } {

        c::log "log_dloop:"
        rule "dloop-$container" $container $turbine::CONTROL $adlb::RANK_ANY 1 \
            "dloop_body $loop_body $stack $container"
    }

    proc dloop_body { loop_body stack container } {

        set keys [ container_list $container ]

        global WORK_TYPE
        foreach key $keys {
            c::log "log_dloop_body"
            set c [ container_lookup $container $key ]
            release "loop_body $loop_body $stack $c"
        }
    }

    proc readdata { result filename } {

        rule "read_data-$filename" $filename $turbine::CONTROL $adlb::RANK_ANY 1 \
            "readdata_body $result $filename"
    }

    proc readdata_body { result filename } {

        set name_value [ retrieve_decr $filename ]
        if { [ catch { set fd [ open $name_value r ] } e ] } {
            error "Could not open file: '$name_value'"
        }

        set i 0
        while { [ gets $fd line ] >= 0 } {
            allocate s string
            store_string $s $line
            container_insert $result $i $s
            incr i
        }
        adlb::slot_drop $result
    }

    # User function
    proc loop { stmts stack container } {
        rule $container "loop_body $stmts $stack $container" \
              type $turbine::CONTROL name "loop-$container" 
    }

    proc loop_body { stmts stack container } {
        set type [ container_typeof $container ]
        set L    [ container_list $container ]
        c::log "loop_body start"
        foreach subscript $L {
            set td_key [ literal $type $subscript ]
            # Call user body with subscript as TD
            # TODO: shouldn't this be an adlb::put ? -Justin
            $stmts $stack $container $td_key
        }
        c::log "log_loop_body done"
    }

    # Utility function to set up a TD
    # usage: [<name>] <type> <value>
    # If name is given, store TD in variable name and log name
    proc literal { args } {

        if { [ llength $args ] == 2 } {
            set type   [ lindex $args 0 ]
            set value  [ lindex $args 1 ]
            set result [ allocate_custom "" $type 1 1 1 ]
        } elseif { [ llength $args ] == 3 } {
            set name   [ lindex $args 0 ]
            set type   [ lindex $args 1 ]
            set value  [ lindex $args 2 ]
            set result [ allocate_custom $name $type 1 1 1 ]
            upvar 1 $name n
            set n $result
        } else {
            error "turbine::literal requires 2 or 3 args!"
        }

        store_${type} $result $value

        return $result
    }

    # User function
    proc toint { result input } {
        rule $input "toint_body $input $result" \
            name "toint-$input" 
    }

    proc toint_body { input result } {
      set t [ retrieve_decr $input ]
      store_integer $result [ check_str_int $t ]
    }

    # Must trim leading zeros - Tcl treats leading zeros as octal
    proc check_str_int { input } {
        if { [ regexp "^0*$" $input  ] } {
            return 0
        }
        if { ! [ string is integer -strict \
                     [ string trimleft $input "0" ] ] } {
            error "could not convert string '${input}' to integer"
        }
        return $input
    }

    proc fromint { result input } {
        rule $input "fromint_body $input $result" \
            name "fromint-$input-$result" 
    }

    proc fromint_body { input result } {
        set t [ retrieve_decr_integer $input ]
        # Tcl performs the conversion naturally
        store_string $result $t
    }

    proc tofloat { result input } {
        rule $input "tofloat_body $input $result" \
            name "tofloat-$input" 
    }

    proc tofloat_body { input result } {
        set t [ retrieve_decr $input ]
        #TODO: would be better if the accepted double types
        #     matched Swift float literals
        store_float $result [ check_str_float $t ]
    }

    proc check_str_float { input } {
      if { ! [ string is double $input ] } {
        error "could not convert string '${input}' to float"
      }
      return $input
    }

    proc fromfloat { result input } {
        rule "fromfloat-$input-$result" $input $turbine::LOCAL $adlb::RANK_ANY 1 \
            "fromfloat_body $input $result"
    }

    proc fromfloat_body { input result } {
        set t [ retrieve_decr $input ]
        # Tcl performs the conversion naturally
        store_string $result $t
    }

    # Good for performance testing
    # c = 0;
    # and sleeps
    proc set0 { c } {
        rule {} "set0_body $c" \
             name "set0-$" type $turbine::WORK 
    }
    proc set0_body { c } {
        log "set0"

        variable stats
        dict incr stats set0

        # Emulate some computation time
        # after 1000
        store_integer $c 0
    }

    # Good for performance testing
    # c = 1;
    # and sleeps
    proc set1 { c } {
        rule {} "set1_body $c" \
             name "set1-$" type $turbine::WORK 
    }
    proc set1_body { c } {
        log "set1"

        variable stats
        dict incr stats set1

        # Emulate some computation time
        # after 1000
        store_integer $c 1
    }

    # Execute shell command DEPRECATED
    proc shell { args } {
        puts "turbine::shell $args"
        set command [ lindex $args 0 ]
        set inputs [ lreplace $args 0 0 ]
        rule "shell-$command" $inputs $turbine::WORK $adlb::RANK_ANY 1 \
            "shell_body $command \"$inputs\""
    }

    proc shell_body { args } {
        set command [ lindex $args 0 ]
        set inputs [ lreplace $args 0 0 ]
        set values [ list ]
        foreach i $inputs {
            set value [ retrieve_decr $i ]
            lappend values $value
        }
        debug "executing: $command $values"
        exec $command $values
    }

    # o = i.  Void has no value, so this just makes sure that
    #         they close sequentially
    proc copy_void { o i } {
        rule $i "copy_void_body $o $i" name "copy-$o-$i" 
    }
    proc copy_void_body { o i } {
        log "copy_void $i => $o"
        store_void $o
        read_refcount_decr $i
    }

    # Copy string value
    proc copy_string { o i } {
        rule $i "copy_string_body $o $i" name "copystring-$o-$i" 
    }
    proc copy_string_body { o i } {
        set i_value [ retrieve_decr_string $i ]
        log "copy $i_value => $i_value"
        store_string $o $i_value
    }

    # Copy blob value
    proc copy_blob { o i } {
        rule $i "copy_blob_body $o $i" name "copyblob-$o-$i" 
    }
    proc copy_blob_body { o i } {
        set i_value [ retrieve_decr_blob $i ]
        log "copy $i_value => $i_value"
        store_blob $o $i_value
        free_blob $i
    }

    # create a void type (i.e. just set it)
    proc make_void { o i } {
        empty i
        store_void $o
    }

    proc zero { outputs inputs } {
        rule $inputs "turbine::zero_body $outputs $inputs" \
            name "zero-$outputs-$inputs" 
    }
    proc zero_body { output input } {
        store_integer $output 0
    }
}

# Local Variables:
# mode: tcl
# tcl-indent-level: 4
# End:
