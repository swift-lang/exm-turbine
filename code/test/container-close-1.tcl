
# Test low-level container close functionality

# SwiftScript
# int c[];
# int i1=0, i2=1;
# int j1=98, j2=72;
# c[i1] = j1;
# c[i2] = j2;
#
# tracef("%k%s", c, "CLOSED!");

package require turbine 0.1

namespace import turbine::data_new
namespace import turbine::string_init
namespace import turbine::integer_*
namespace import turbine::literal
namespace import turbine::enumerate
namespace import turbine::c::rule
namespace import turbine::c::rule_new

proc rules { } {

    set c [ data_new ]
    turbine::container_init $c integer

    set i1 [ literal integer 0 ]
    set i2 [ literal integer 1 ]

    set j1 [ literal integer 98 ]
    set j2 [ literal integer 72 ]

    turbine::container_f_insert no_stack "" "$c $i1 $j1"
    turbine::container_f_insert no_stack "" "$c $i2 $j2"

    set rule_id [ rule_new ]
    rule 1 1 "$c" "" "tp: puts CLOSED!"
}

turbine::init $env(TURBINE_ENGINES) $env(ADLB_SERVERS)
turbine::start rules
turbine::finalize

puts OK

# Help TCL free memory
proc exit args {}
