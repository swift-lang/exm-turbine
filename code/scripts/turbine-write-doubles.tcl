
# TURBINE-WRITE-DOUBLES

# Writes a file of doubles from command line arguments

# Usage: turbine-write-doubles.tcl <output> <doubles...>

package require turbine 0.1.1

if { $argc < 1 } {
    puts "requires output file name!"
    exit 1
}

set output [ lindex $argv 0 ]
# puts "output file: $output"

# Do a shift
incr argc -1
set argv [ lreplace $argv 0 0 ]

set sizeof_double [ blobutils_sizeof_float ]

set length [ expr $argc * $sizeof_double ]

set ptr [ blobutils_malloc $length ]

set blob [ blobutils_create_ptr $ptr $length ]

for { set i 0 } { $i < $argc } { incr i } {
    set v [ lindex $argv $i ]
    blobutils_set_float $ptr $i $v
}

blobutils_write $output $blob
