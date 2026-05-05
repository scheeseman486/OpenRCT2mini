# Contributing to OpenRCT2mini
Any contribution to OpenRCT2mini is welcome and valued. Contributions can be in the form of bug reports/fixes or ports to
new hardware. Gameplay bugs should also be attempted to be replicated on the latest official, upstream OpenRCT2 release and
if replicated, should be handled upstream instead.

# Reporting bugs

To report a bug, ensure you have a GitHub account. Search the issues page to see if the bug has already been reported.
If not, create a new issue and write the steps to reproduce. Upload a saved game if possible as this is very helpful
for users to replicate the bug. Please state which architecture and version of the game you are running, e.g.

This can be found at the bottom left of the title screen.

# Contributing code
This port was heavily vibe coded so I'm not expecting Carmack-level finesse, but try not to mess anything up too bad. We
target C++17 (which was mechanically backported using Claude) due to targeting an older toolchain.
