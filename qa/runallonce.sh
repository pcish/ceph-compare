#!/bin/bash -x

set -e

basedir=`echo $0 | sed 's/[^/]*$//g'`.
testdir="$1"

test -d $testdir || ( echo "specify test dir" && exit 1 )
cd $testdir

for test in `cd $basedir && find workunits/* | grep .sh`
do
  echo "------ running test $test ------"
  mkdir -p $test
  pushd .
  cd $test
  ${basedir}/${test}
  popd
done