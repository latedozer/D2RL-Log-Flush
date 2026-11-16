#!/bin/bash
# Builds the plugin and the stand-ins, then runs the checks under Wine (twice: on, and enabled = false).
set -e
cd "$(dirname "$0")/.."
SDK="${SDK:-../PluginSDK-master/include}"
./build.sh > /dev/null
x86_64-w64-mingw32-g++ -std=c++20 -O2 -shared -static -static-libgcc -static-libstdc++ -o build/D2RCore.dll test/fake_core.cpp
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wno-missing-field-initializers -static -static-libgcc -static-libstdc++ -I "$SDK" -o build/fake_host.exe test/fake_host.cpp
status=0
for mode in on disabled; do
	WORK="$(mktemp -d)"
	echo "== $mode"
	( cd build && WINEDEBUG=-all timeout 120 wine fake_host.exe "$(winepath -w "$PWD/d2rl-background-log-flush.dll")" "$(winepath -w "$WORK")" $mode 2>/dev/null ) \
		| grep -E '^(ok|FAIL|ALL|FAILURES|  \[)' || status=1
	rm -rf "$WORK"
done
exit $status
