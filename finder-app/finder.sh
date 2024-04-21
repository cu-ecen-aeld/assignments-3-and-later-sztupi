#!/usr/bin/env sh

if [ $# -lt 1 ]; then
	echo "argument filesdir missing"
	exit 1
fi

filesdir=$1
shift

if [ ! -d "$filesdir" ]; then
	echo "filesdir $filesdir is not a directory"
	exit 1
fi


if [ $# -lt 1 ]; then
	echo "argument searchstr missing"
	exit 1
fi

searchstr=$1
shift


match_files=$(grep -R "$searchstr" $filesdir -c | wc | awk '{ print $1; }')
match_count=$(grep -R "$searchstr" $filesdir -c | awk 'BEGIN {FS=":"}; /:.*/ { sum += $2 } END { print sum }')

echo "The number of files are $match_files and the number of matching lines are $match_count"

