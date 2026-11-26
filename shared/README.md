# Shared code

Code in this directory is compiled into the client, daemon, or both. It holds
the inference/model wrapper, prompts, JSON parsing, diff filtering, decision
logic, and RAG implementation. Keep UI, local Git command handling, and HTTP
process entry points in their respective `client/` and `daemon/` directories.
