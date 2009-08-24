#!/bin/sh
#
# Author: Amos Jeffries <squid3@treenet.co.nz>
#
# This code is copyright (C) 2009 by Treehouse Networks Ltd
# of New Zealand. It is published and Licensed as an extension of
# squid under the same conditions as the main squid application.
#

if test "${1}" = "-h" ; then
	echo "Usage: $0 [-h] [-c] [-d logfile]"
	echo "  -h           Help: this help text"
	echo "  -c           Accept concurrent request format"
	echo "  -d logfile   Debug: log all data received to the named file"
	exit 1
fi

concurrent=0
if test "${1}" = "-c" ; then
	concurrent=1
	shift
fi

DEBUG=0
if test "${1}" = "-d" ; then
	DEBUG=1
	LOG="${2}"
fi

if test "$concurrent" = "1"; then
	while read id url rest; do
		if test ${DEBUG} ; then
			echo "ID:$id URL:$url EXTRAS:$rest" >>${LOG}
		fi
		echo "${id} " # blank URL for no change, or replace with another URL.
	done
else
	while read url rest; do
		if test ${DEBUG} ; then
			echo "URL:$url EXTRAS:$rest" >>${LOG}
		fi
		echo  # blank line/URL for no change, or replace with another URL.
	done
fi
