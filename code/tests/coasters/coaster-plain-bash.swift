import files;

app (file out, file err) bashing (file foo) {
    "/bin/bash" foo @stderr=err @stdout=out;
}


/**
 * Test coaster output file creation and passing arguments functionality
 * Test capability to run multiple jobs (this should just work)
 * Test capability to pass files as args
 */
main(){
    string msg = "-f";
    string dir = "/homes/yadunand/bin/";
    file script = input_file("/homes/yadunand/bin/exm-trunk/sfw/turbine/branches/issue-503/code/tests/coasters/wrapper.sh");
    file f_out<strcat(dir, "test5.0.out")>;
    file f_err<strcat(dir, "test5.0.err")>;
    (f_out, f_err) = bashing (script);
}
