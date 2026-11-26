# Hosted API backend

This directory contains the container and Render Blueprint for the C++ HTTP API.
The API implementation is built as the separate `mygit_daemon` target. Shared
model and review logic is in `shared/`; client-only commands and terminal UI
are in `client/`.

## Current behavior

Hosted mode is an authenticated **stub only**. It validates API transport and
returns synthetic results; it does not run the local GGUF model. Configure
`MYGIT_API_TOKEN` as a Render secret (at least 24 characters). Do not present
this deployment as real code review inference.

The Render Blueprint is `daemon/render.yaml`. It uses the repository root as
the Docker build context, so source paths in the Dockerfile remain valid.

See [deployment notes](DEPLOYMENT.md) for the current boundary and future work.
