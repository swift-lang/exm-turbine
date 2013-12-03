import files;
import string;
app (file out, file err) bashing (file foo) {
    "coaster/bin/bash" foo @stderr=err @stdout=out;
}

/**
 * Test multiplexing
 */
main(){

    file script = input_file("/homes/yadunand/bin/exm-trunk/sfw/turbine/branches/issue-503/code/tests/coasters/wrapper.sh");
    foreach index in [0:10]
    {
        file f_out <sprintf("/homes/yadunand/bin/test%i.out", index)>;
        file f_err <sprintf("/homes/yadunand/bin/test%i.err", index)>;
        (f_out, f_err) = bashing (script);
    }
}
