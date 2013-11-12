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

# Turbine IO.TCL

# Turbine I/O library routines

namespace eval turbine {

    # namespace export printf

    # A single output can optionally be provided, which is a void
    # variable used to signal that printing is complete
    proc printf { outputs inputs  } {
        set signal [ lindex $outputs 0 ] 
        rule $inputs "printf_body {$signal} $inputs" name "printf"
    }
    proc printf_body { signal args } {
        set L [ list ]
        foreach a $args {
            lappend L [ retrieve_decr $a ]
        }
        set s [ eval format $L ]
        puts $s

        if { ! [ string equal $signal "" ] } {
          store_void $signal
        }
    }

    proc printf_local { args } {
        set L [ list ]
        foreach a $args {
            lappend L $a
        }
        set s [ eval format $L ]
        puts $s
        return 0
    }
}
