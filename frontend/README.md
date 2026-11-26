# Frontend / demo client

This folder is reserved for a browser based demo UI. The current project has a
terminal UI (`ui/`) and a hosted HTTP API, but no browser frontend yet.

A future frontend can be deployed independently and configured with the hosted
API URL. It must never contain `MYGIT_API_TOKEN`; add a server-side proxy or
user authentication before allowing browser clients to call a token-protected
backend. The current backend is a stub and does not perform real inference.
