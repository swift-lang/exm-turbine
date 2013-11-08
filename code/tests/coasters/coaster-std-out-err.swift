import files;

app (file out, file err) date () {
    "coaster/bin/date" @stderr=err @stdout=out;
}

/**
 * Test coaster output file location functionality
 */
main()
{
    file f_out<"/homes/yadunand/test2.out">;
    file f_err<"/homes/yadunand/test-2.err">;
    (f_out, f_err) = date();
}
