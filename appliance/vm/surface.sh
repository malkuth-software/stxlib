#!/bin/sh
# surface.sh -- the run-time half of the surface inventory (C2,
# appliance/C2_DESIGN.md, "Surface inventory"), installed only in the VM
# scenario's root as /etc/stx/surface.sh and run by its service "surface"
# once targets and services are up. It prints every entry devfs shows
# (three levels: devfs has no deeper names on these machines, and a fourth
# level is reported if one appears) and every socket sockstat(1) lists as
# listening. appliance/vm/check_surface.py holds both lists to what the
# configuration allows, every line.
/bin/sleep 20
for path in /dev/* /dev/*/* /dev/*/*/*; do
    [ -e "$path" ] || [ -L "$path" ] || continue
    echo "STXSURFACE dev ${path#/dev/}"
done
for path in /dev/*/*/*/*; do
    [ -e "$path" ] || [ -L "$path" ] || continue
    echo "STXSURFACE deeper ${path#/dev/}"
done
/usr/bin/sockstat -l | while read -r line; do
    echo "STXSURFACE listen $line"
done
echo "STXSURFACE done"
