#!/usr/bin/env sh

if [ $# -lt 1 ]; then
	echo "argument writefile missing"
	exit 1
fi

writefile=$1
shift

if [ $# -lt 1 ]; then
	echo "argument writestr missing"
	exit 1
fi

writestr=$1
shift

mkdir -p $(dirname $writefile)

echo "$writestr" > $writefile
