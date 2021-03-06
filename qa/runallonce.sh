#!/bin/bash -x

set -e

basedir=`echo $0 | sed 's/[^/]*$//g'`.
testdir="$1"
[ -n "$2" ] && logdir=$2 || logdir=$1

[ ${basedir:0:1} == "." ] && basedir=`pwd`/${basedir:1}

[ -z "$testdir" ] || [ ! -d "$testdir" ] && echo "specify test dir" && exit 1
cd $testdir

for test in `cd $basedir/workunits && ls | grep .sh | $basedir/../src/script/permute`
do
  echo "------ running test $test ------"
  pwd
  [ -d $test ] && rm -r $test
  mkdir -p $test
  mkdir -p `dirname $logdir/$test.log`
  test -e $logdir/$test.log && rm $logdir/$test.log
  sh -c "cd $test && $basedir/workunits/$test" 2>&1 | tee $logdir/$test.log
done
