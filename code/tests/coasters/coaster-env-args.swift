import files;

app (file out, file err) env () {
    "coaster/usr/bin/env X=x" @stderr=err @stdout=out;
}

app (file out, file err) test () {
    "coaster/bin/hostname" @stderr=err @stdout=out;
}


/**
 * Test coaster env variable functionality
 * Sadly the coasters system does not like env variable prepended to the
 * front of the executable that it expected the cmd varible to be
 */
main(){
    string msg = "-f";
    file f_out<"/homes/yadunand/bin/exm-trunk/sfw/turbine/trunk/code/tests/coasters/f4.out">;
    file f_err<"/homes/yadunand/bin/exm-trunk/sfw/turbine/trunk/code/tests/coasters/f4.err">;
    (f_out, f_err) = test ();
}
