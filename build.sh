#!/bin/bash
set -e
CXX=aarch64-linux-gnu-g++-14; CC=aarch64-linux-gnu-gcc-14
ROOT=$(pwd); SRC=$ROOT/source; B=$ROOT/build; mkdir -p $B
PROGRAM_ID=${PROGRAM_ID:-0100000000010000}
MODS=$(find $SRC -mindepth 1 -maxdepth 1 -type d | sed 's/^/-I/')
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIC -fvisibility=hidden -fno-stack-protector -fno-plt -mno-outline-atomics -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
CFLAGS="-g -Wall -O3 -ffunction-sections -fdata-sections -Wno-format-zero-length $ARCH -D__SWITCH__ -D__RTLD_6XX__ -DEXL_LOAD_KIND=Module -DEXL_LOAD_KIND_ENUM=2 -DEXL_PROGRAM_ID=0x$PROGRAM_ID -I$SRC $MODS"
CXXFLAGS="$CFLAGS -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables -std=gnu++23"
objs=()
while IFS= read -r f; do
  o=$B/$(echo ${f#$SRC/} | tr '/' '_').o
  case $f in
    *.cpp) $CXX $CXXFLAGS -c $f -o $o ;;
    *.c)   $CC $CFLAGS -std=gnu11 -c $f -o $o ;;
    *.s|*.S) $CC $ARCH -c $f -o $o ;;
  esac
  objs+=($o)
done < <(find $SRC -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.s' -o -name '*.S' \))
$CXX -nostdlib -nostartfiles -Wl,-T,$ROOT/misc/link.ld -Wl,--shared -Wl,--export-dynamic -Wl,-z,nodynamic-undefined-weak \
  -Wl,--gc-sections -Wl,--build-id=sha1 -Wl,-init=exl_module_init -Wl,--exclude-libs=ALL -Wl,-z,max-page-size=0x1000 -Wl,--hash-style=sysv \
  -Wl,-Map,$B/subsdk9.map -o $B/subsdk9.elf "${objs[@]}" -lgcc
echo built $B/subsdk9.elf
