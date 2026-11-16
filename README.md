# Background Log Flush

A D2RLoader plugin that stops plugin log lines from freezing the game.

D2RLoader (in `D2RCore.dll`) writes each plugin's log line, then flushes the file to disk on the
thread that logged it. On a slow drive one flush took 0.25-1 s, and the game stood still that long
(found with Lag Finder: freezes ending within 1 ms of a log line, the frame thread inside
D2RCore's log writer).

With this plugin each line is still written straight away and in order; only the flush is done
on a background thread. It waits `batch_ms` (500 by default) and then flushes each log file once,
so a burst of lines costs one flush per file. A line that is written is with the system, so it
reaches the file even if the game crashes; only a power cut in that half second could lose it.

How: D2RCore's import of `FlushFileBuffers` is pointed at the plugin. Each flush D2RCore makes is
checked by file name. A `.log` file in a `d2rloader\logs` folder goes to the background thread;
everything else (settings, saves, crash files) is flushed as before, before the call returns.
Nothing about D2RCore's code is hard-coded. If D2RCore ever stops importing `FlushFileBuffers`
by name, the plugin does nothing and says so in its log.

## Install

`./build.sh`, then `./install.sh` (copies the DLL into `Diablo II Resurrected/d2rloader/plugins/`;
refuses while the game runs; `./install.sh remove` takes it out).

Settings: `d2rloader/config/background-log-flush.toml` (`enabled`, `batch_ms`).
Console (Ctrl + `): `logflush` shows how many lines did not wait for the disk, how many
background flushes there were and the slowest one (what the game would have waited).
Its own log, `d2rloader/logs/background-log-flush.log`, says at start-up whether it found the log writer.

## Test

`test/run_tests.sh` (mingw-w64, Wine, the Plugin SDK headers at `../PluginSDK-master/include` or `SDK=`).
It builds a stand-in `D2RCore.dll` that writes log lines and a save file the way D2RCore does,
makes every flush take 300 ms, loads the plugin as D2RLoader does and checks: lines no longer
wait, all lines are in the file in order, 30 lines cost a couple of flushes, the save still waits
for its flush, lines from other threads, the console report, unloading (import put back, last
lines flushed), and `enabled = false`.
