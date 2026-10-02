#!/bin/sh

case "$#" in
    0)  P="CaenV975Test:" R="CaenV965:"   ;;
    1)  P="$1"            R="CaenV965:"   ;;
    2)  P="$1"            R="$2"          ;;
    *)  echo "Usage: $0 [P [R]]" >&2 ; exit 1 ;;
esac

camonitor "${P}${R}FirmwareRev"  \
          "${P}${R}SerialNo"     \
          "${P}${R}Version"      \
          "${P}${R}MotherRev"    \
          "${P}${R}PiggyRev"     \
          "${P}${R}Iped"
