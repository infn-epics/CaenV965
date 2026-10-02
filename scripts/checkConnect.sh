#!/bin/sh

trap 'kill $pid; exit 2' 1 2 3
while [ true ]
do
    camonitor CaenV965Test:CaenV965:dac0 CaenV965Test:CaenV965:EventCount &
    pid=$!
    sleep 1
    kill -HUP $pid
    sleep 1
done


