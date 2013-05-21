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
# Turbine builtin functions for blob manipulation

namespace eval turbine {

  proc blob_size_async { out blob } {
    rule "$blob" "blob_size_body $out $blob" \
        name "blob_size-$out-$blob"
  }

  proc blob_size_body { out blob } {
    set blob_val [ retrieve_decr_blob $blob ]
    set sz [ blob_size $blob_val ]
    store_integer $out $sz
    adlb::blob_free $blob
  }

  proc blob_size { blob_val } {
    return [ lindex $blob_val 1 ]
  }

  proc blob_null { result input } {
      store_blob $result 0 0
  }

  proc blob_from_string { result input } {
    rule $input "blob_from_string_body $input $result" \
        name "bfs-$input-$result"
  }
  proc blob_from_string_body { input result } {
    set t [ retrieve_decr $input ]
    store_blob_string $result $t
  }

  proc string_from_blob { result input } {
    rule $input "string_from_blob_body $input $result" \
        name "sfb-$input-$result"
  }
  proc string_from_blob_body { input result } {
    set s [ retrieve_decr_blob_string $input ]
    store_string $result $s
  }

  proc floats_from_blob { result input } {
      rule $input "floats_from_blob_body $result $input" \
          name "floats_from_blob-$result"
  }
  proc floats_from_blob_body { result input } {
      log "floats_from_blob_body: result=<$result> input=<$input>"
      set s      [ blobutils_sizeof_float ]
      set L      [ adlb::retrieve_blob $input ]
      set p      [ blobutils_cast_int_to_dbl_ptr [ lindex $L 0 ] ]
      set length [ lindex $L 1 ]

      set n [ expr {$length / $s} ]
      for { set i 0 } { $i < $n } { incr i } {
          set d [ blobutils_get_float $p $i ]
          literal t float $d
          container_immediate_insert $result $i $t
      }
      adlb::refcount_incr $result $::adlb::WRITE_REFCOUNT -1
      adlb::blob_free $input
      log "floats_from_blob_body: done"
  }

  # This is just in Fortran order for now
  # b: the blob
  # m: number of rows
  # n: number of columns
  proc matrix_from_blob_fortran { result inputs } {
      set b [ lindex $inputs 0 ]
      set m [ lindex $inputs 1 ]
      set n [ lindex $inputs 2 ]
      rule [ list $b $m $n ] \
          "matrix_from_blob_fortran_body $result $inputs" \
          name "matrix_from_blob-$result"
  }
  proc matrix_from_blob_fortran_body { result b m n } {
      log "matrix_from_blob_fortran_body: result=<$result> blob=<$b>"
      # Retrieve inputs
      set s       [ blobutils_sizeof_float ]
      set L       [ retrieve_blob $b ]
      set p       [ blobutils_cast_int_to_dbl_ptr [ lindex $L 0 ] ]
      set m_value [ retrieve_integer $m ]
      set n_value [ retrieve_integer $n ]
      set length  [ lindex $L 1 ]

      # total = m x n
      # i is row index:       0..m-1
      # j is column index:    0..n-1
      # k is index into blob: 0..total-1
      # c[i] is row result[i]
      set total [ expr {$length / $s} ]
      if { $total != $m_value * $n_value } {
          error "matrix_from_blob: blob size $total != $m_value x $n_value"
      }
      for { set i 0 } { $i < $m_value } { incr i } {
          set c($i) [ allocate_container $::adlb::INTEGER ]
          container_immediate_insert $result $i $c($i)
      }
      for { set k 0 } { $k < $total } { incr k } {
          set d [ blobutils_get_float $p $k ]
          literal t float $d
          set i [ expr {$k % $m_value} ]
          set j [ expr {$k / $m_value} ]
          container_immediate_insert $c($i) $j $t
      }
      # Close rows
      for { set i 0 } { $i < $m_value } { incr i } {
          adlb::refcount_incr $c($i) $::adlb::WRITE_REFCOUNT -1
      }
      # Close result
      adlb::refcount_incr $result $::adlb::WRITE_REFCOUNT -1
      # Release cached blob
      adlb::blob_free $b
      log "matrix_from_blob_fortran_body: done"
  }

  # Container must be indexed from 0,N-1
  proc blob_from_floats { result input } {
    rule $input  "blob_from_floats_body $input $result" \
        name "blob_from_floats-$result"
  }
  proc blob_from_floats_body { container result } {

      set type [ container_typeof $container ]
      set N  [ adlb::container_size $container ]
      c::log "blob_from_floats_body start"
      complete_container $container \
          "blob_from_floats_store $result $container $N"
  }
  # This is called when every entry in container is set
  proc blob_from_floats_store { result container N } {
    set A [ list ]
    for { set i 0 } { $i < $N } { incr i } {
      set td [ container_lookup $container $i ]
      set v  [ retrieve_decr_float $td ]
      lappend A $v
    }
    set waiters [ adlb::store_blob_floats $result $A ]
    turbine::notify_waiters $result $waiters
  }

  # Container must be indexed from 0,N-1
  proc blob_from_ints { result input } {
    rule $input "blob_from_ints_body $input $result" \
        name "blob_from_ints-$result"
  }
  proc blob_from_ints_body { container result } {

      set type [ container_typeof $container ]
      set N  [ adlb::container_size $container ]
      c::log "blob_from_ints_body start"
      complete_container $container \
          "blob_from_ints_store $result $container $N"
  }
  # This is called when every entry in container is set
  proc blob_from_ints_store { result container N } {
    set A [ list ]
    for { set i 0 } { $i < $N } { incr i } {
      set td [ container_lookup $container $i ]
      set v  [ retrieve_decr_integer $td ]
      lappend A $v
    }
    set waiters [ adlb::store_blob_ints $result $A ]
    turbine::notify_waiters $result $waiters
  }

  # Assumes A is closed
  proc complete_container { A action } {
      set n [ adlb::container_size $A ]
      log "complete_container: <$A> size: $n"
      complete_container_continue $A $action 0 $n
  }
  proc complete_container_continue { A action i n } {
      log "complete_container_continue: <$A> $i/$n"
      if { $i < $n } {
          set x [ container_lookup $A $i ]
          if { $x == 0 } {
              error "complete_container: <$A>\[$i\]=<0>"
          }
          rule [ list $x ] \
              "complete_container_continue_body $A {$action} $i $n" \
              name "complete_container_continue-$A"
      } else {
          eval $action
      }
  }
  proc complete_container_continue_body { A action i n } {
      complete_container_continue $A $action [ incr i ] $n
  }
}
