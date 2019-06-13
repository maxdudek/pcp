#! /bin/sh
# PCP QA Test No. ???
# test dmi info fetch

seq=`basename $0`
echo "QA output created by $seq"

# get standard environment, filters and checks
. ./common.product
. ./common.filter
. ./common.check

[ $PCP_PLATFORM = linux ] || _notrun "Linux cpuinfo test, only works with Linux"

status=1	# failure is the default!
$sudo rm -rf $tmp.* $seq.full
trap "cd $here; rm -rf $tmp.*; exit \$status" 0 1 2 3 15

# real QA test starts here
scriptpath="$( cd "$(dirname "$0")" ; pwd -P )"
export DMI_TEST_PATH="$scriptpath/linux/dmi_test/"
pminfo -f hinv.dmi