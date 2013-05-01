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

#LOCATION.tcl
# Functions to deal with placement of computation in the cluster

namespace eval turbine {

  proc random_worker { } {
    # Engines are allocated to first ranks
    set eng [ turbine_engines ]
    return [ randint_impl $eng [ expr {$eng + [ turbine_workers ]} ] ]
  }

  proc random_engine { } {
    # Engines are allocated to first ranks
    return [ randint_impl 0 [ turbine_engines ] ]
  }

  proc check_rank { rank } {
    if { $rank < 0 || $rank >= [ adlb::size ] } {
      error "Rank out of range: ${rank}"
    }
  }

  proc random_rank { type ranklist } {
    set filtered [ list ]
    switch $type {
      WORKER {
        foreach rank $ranklist {
          if [ rank_is_worker $rank ] {
            lappend filtered $rank
          }
        }
      }
      ENGINE {
        foreach rank $ranklist {
          if [ rank_is_engine $rank ] {
            lappend filtered $rank
          }
        }
      }
      SERVER {
        foreach rank $ranklist {
          if [ rank_is_server $rank ] {
            lappend filtered $rank
          }
        }
      }
      default {
        error "Unknown type $type"
      }
    }
    if { [ llength $filtered ] == 0 } {
      error "No ranks of type $type in list: $ranklist"
    }
    return [ draw $filtered ]
  }

  proc rank_is_worker { rank } {
    check_rank $rank
    set eng [ turbine_engines ]
    return [ expr {$rank >= $eng && $rank < $eng + [ turbine_workers ] } ]
  }

  proc rank_is_engine { rank } {
    check_rank $rank
    return [ expr {$rank < [ turbine_engines ] } ]
  }

  proc rank_is_server { rank } {
    check_rank $rank
    # servers are allocated to topmost ranks
    return [ expr {$rank >= [ turbine_engines ] + [ turbine_workers ]} ]
  }
}
