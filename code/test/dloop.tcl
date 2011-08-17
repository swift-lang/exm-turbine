
# Test distributed loop functionality

# SwiftScript
# int i = 1;
# int j = 4;
# int c[] = [i:j];
# foreach v in c
#   trace(v);

package require turbine 0.1

proc rules { } {

    set i [ turbine::data_new ]
    turbine::integer_init $i
    set j [ turbine::data_new ]
    turbine::integer_init $j
    set c [ turbine::data_new ]
    turbine::container_init $c integer
    set p [ turbine::data_new ]
    turbine::integer_init $p

    global env
    if { [ info exists env(COUNT) ] } {
        set count $env(COUNT)
    } else {
        set count 10
    }
    puts "COUNT: $count"

    turbine::integer_set $i 1
    turbine::integer_set $j $count
    set split [ expr $env(TURBINE_ENGINES) * 10 ]
    turbine::integer_set $p $split

    turbine::drange $c $i $j $p
    turbine::dloop loop1_body none $c
}

proc loop1_body { stack container key } {
    # puts "loop1_body: $key"
    set t [ turbine::integer_get $key ]
    set member [ turbine::container_get $container $t ]
    set value [ turbine::integer_get $member ]
    # turbine::trace $key $member
    # puts "value: $value"
}

global env
turbine::init $env(TURBINE_ENGINES) $env(ADLB_SERVERS)
turbine::start rules
turbine::finalize
puts OK
