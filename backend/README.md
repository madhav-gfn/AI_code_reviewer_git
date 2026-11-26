# Hosted API backend

This directory contains the container and Render Blueprint for the C++ HTTP API.
The API implementation remains in the shared C++ source tree (`daemon/` and
`commands/`) and is built from the repository root through `backend/Dockerfile`.

## Current behavior

Hosted mode is an authenticated **stub only**. It validates API transport and
returns synthetic results; it does not run the local GGUF model. Configure
`MYGIT_API_TOKEN` as a Render secret (at least 24 characters). Do not present
this deployment as real code review inference.

The Render Blueprint is `backend/render.yaml`. It uses the repository root as
the Docker build context, so source paths in the Dockerfile remain valid.

See [deployment notes](DEPLOYMENT.md) for the current boundary and future work.
