#!/bin/sh

enable_ipv6()
{
    echo 0 > /proc/sys/net/ipv6/conf/all/disable_ipv6
}

disable_ipv6()
{
    echo 1 > /proc/sys/net/ipv6/conf/all/disable_ipv6
}

if [ "$1" = "1" ] ; then
    disable_ipv6
elif [ "$1" = "0" ] ; then
    enable_ipv6
fi