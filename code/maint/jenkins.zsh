#!/bin/zsh

./setup.sh

C_UTILS=/tmp/exm-install/c-utils
TURBINE=/tmp/exm-install/turbine
STC=/tmp/exm-install/stc
MPICH=/tmp/mpich-install
path+=( $MPICH/bin $TURBINE/bin $STC/bin )

set -x

# printenv

ls /tmp/mpich-install/lib

LDFLAGS="-L$MPICH/lib -lmpl"                \
./configure --prefix=$TURBINE               \
            --with-tcl=/usr                 \
            --with-mpi=$MPICH               \
            --with-c-utils=$C_UTILS         \
            --with-adlb=/tmp/exm-install/lb \
            --enable-shared
make clean

make V=1

make test_results
# Make should always return 0 under Jenkins even if a test failed
# We inpect test results below

cd tests
SUITE_RESULT="result_aggregate.xml";
rm -fv $SUITE_RESULT

inspect_results()
{
  print "<testsuites>"
  for result in *.result
  do
    grep "ERROR" ${result} > /dev/null
    CODE=${?}

    if [[ ${CODE} == 0 ]]
    then
      # We found ERROR
      print "    <testcase name=\"${result}\" >"
      print "        <failure type=\"generic\">"
      print "Result file contents:"
      cat $result
      print ""
      print ""
      print "Out file contents:"
      print "<![CDATA["
      cat ${result%.result}.out
      print "]]>"
      print "        </failure> "
      print "    </testcase>"
    else
      # Success:
      print "    <testcase name=\"${result}\" />"
    fi
  done
  print "</testsuites>"
}

inspect_results > ${SUITE_RESULT}
