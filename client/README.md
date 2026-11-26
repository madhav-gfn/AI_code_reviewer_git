# Client application

This directory contains the executable and code that runs on the user's
computer: CLI commands, Git access, local configuration/database, client-side
daemon management, and terminal UI. It calls either a local daemon or a remote
hosted API using `MYGIT_BACKEND`.

Build the project from the repository root with `cmake -S . -B build` followed
by `cmake --build build`. This produces `mygit` and `mygit_daemon`. A local
client installation needs both executables: `mygit` launches the companion
`mygit_daemon` for local inference. The hosted container installs only the
daemon component.
