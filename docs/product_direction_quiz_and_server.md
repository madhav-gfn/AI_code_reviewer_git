# Product direction: hosted review service and push quiz

## Executive summary

`mygit` can grow in two connected directions:

1. **Deploy a server** that provides shared, persistent review and quiz inference instead of requiring every developer to host the model locally.
2. **Add a comprehension quiz before push**. For every commit about to leave the developer's machine, ask a small set of questions grounded in its changes and the repository's existing behavior. Push proceeds only after the developer demonstrates they understand the change.

The quiz is a comprehension check, not a correctness verdict or a replacement for code review. The existing review can continue to report risks; the quiz checks whether the person pushing can explain what changed, why it works, and how it interacts with the surrounding code.

## What exists today

- `mygit push` currently reads the **staged diff**, reviews it locally with `LlamaClient`, and runs `git push` if the decision engine allows it. See `commands/push_command.cpp`.
- This does not represent all outgoing commits. A push can include already committed changes that are not staged, and may have no staged changes at all.
- The local HTTP daemon binds to `127.0.0.1`, holds a local `LlamaClient`, and exposes review/commit work to the CLI. The server process is in `daemon/`; client transport is in `client/daemon_client.cpp`.
- Repository context building blocks exist in `rag/`: Tree-sitter code units, text chunks, embeddings, and retrieval. They can supply nearby symbols and existing code to quiz generation, subject to the configured embedding assets being available.
- The current types and prompt paths focus on review issues and commit messages. There is no quiz session, question/answer API, quiz persistence, or push-range collection yet.

## Product goals

- Quiz **every commit included in the push**, including commits already made locally.
- Generate questions from concrete changed lines and symbols, not generic programming trivia.
- Use the base branch's existing code and relevant callers/callees as context, so questions test how the change fits the project.
- Require answers before the push proceeds; keep the interaction short and explain incorrect answers.
- Make the service deployable while keeping repository data access explicit and minimized.
- Preserve a useful local-only mode for developers who do not want to send source to a hosted service.

## Non-goals

- Proving authorship or reliably detecting AI-generated code.
- Replacing tests, review, CI, or human approval.
- Asking trivia unrelated to the pushed diff.
- Sending an entire repository to a hosted model when a bounded set of changed files and retrieved context is enough.

## Proposed user experience

```text
$ mygit push origin feature/cache
Collecting outgoing commits: 3 commits, 8 changed files
Preparing comprehension quiz...

1/4 What condition makes Cache::get return a miss after this change?
> ...
Correct. The entry is expired when now >= expires_at.

2/4 The new invalidation path is called from which existing operation?
> ...
Not quite. It is called after Cache::erase removes the key.

Quiz passed (3/4). Pushing 3 commits...
```

The default should be a short quiz (for example, 3–5 questions) with a configurable passing threshold. Questions should be selected from the actual change surface, with coverage across commits and important symbols. If there is not enough meaningful code change to quiz (for example, a merge-only or documentation-only push), the policy must be explicit: skip with a recorded reason, or require a configured minimum question count. Do not silently invent generic questions.

## Correct Git scope: outgoing commits, not the index

Before quiz generation, resolve the push ref and remote tracking state, then compute the exact commits and file changes that the push would send. The conceptual range is `remote-tracking-base..local-head`; force pushes, deleted refs, multiple refspecs, shallow clones, and missing tracking refs need explicit handling. Prefer libgit2 or a carefully constrained Git invocation that resolves the same refspec Git will push.

For each outgoing commit, collect its ID, parent/base relationship, message, changed paths, status (add/modify/delete/rename), and per-file patch. Keep commit boundaries so questions can be tied back to a specific commit. Do not include unstaged/uncommitted working-tree changes in the quiz unless a future product mode explicitly asks to quiz them.

The existing staged-diff flow can remain useful for `mygit review` and commit-time checks, but it is the wrong input for a push gate.

## Semantic question generation

### Context assembly

For each outgoing change:

1. Parse the diff into changed hunks and map changed lines to symbols where the language parser supports it.
2. Read the relevant file contents at the pushed commit, rather than accidentally using the developer's later uncommitted working tree.
3. Retrieve a bounded set of related existing symbols: callers, callees, declarations, related tests, and nearby invariants. The current RAG index is a useful retrieval aid, but the authoritative text should be checked out/read from the correct Git tree.
4. Include the parent/base implementation where behavior changed, plus concise commit metadata. Exclude secrets, generated/vendor files, binary contents, and unrelated repository material.
5. Produce questions with evidence references: commit ID, path, symbol, and changed line range. Evidence is used to validate relevance and explain answers.

When the repository is hosted remotely, the server cannot read the developer's working tree by itself. The client must send a bounded, authenticated quiz request containing outgoing commits' patches and enough versioned context, or the server must fetch a repository snapshot using a narrowly scoped integration. The default deployment design should prefer the client-supplied bounded payload; a remote server should not receive arbitrary filesystem access.

### Useful question types

- **Behavior:** What does the changed function return in a particular condition?
- **Control/data flow:** Which new branch runs when a boundary case occurs, and what state is changed?
- **Integration:** Which existing caller or contract depends on the changed behavior?
- **Invariant/failure mode:** What prevents a stale entry, null value, duplicate request, or resource leak?
- **Test intent:** Which behavior does a new/changed test protect, and what case remains uncovered?

Questions should be answerable from the patch plus the cited repository context. Avoid ambiguous questions, style preferences, and claims that require hidden runtime facts. Generation should return structured data (question, answer rubric, accepted variants, evidence, commit ID, difficulty, and topic). Validate every question against its evidence before presenting it; discard unsupported or duplicate questions.

### Answer evaluation

Use a short free-text answer initially, with deterministic checks where possible and model grading against a narrow rubric for semantic answers. Ask one question at a time, provide concise corrective feedback, and do not expose the answer before the attempt. Store only what is needed to resume/report the quiz. The score should reflect understanding across the outgoing change set, not exact wording.

## Push-gate flow

```mermaid
sequenceDiagram
    participant Dev as Developer
    participant CLI as mygit client
    participant Git as Git repository
    participant Service as Quiz/review service
    participant Model as Inference runtime
    Dev->>CLI: mygit push remote branch
    CLI->>Git: Resolve refspec and enumerate outgoing commits
    Git-->>CLI: Commit IDs, per-commit patches, paths
    CLI->>Service: Authenticated bounded quiz request
    Service->>Model: Generate grounded questions and rubrics
    Model-->>Service: Candidate questions with evidence
    Service-->>CLI: Validated quiz session
    loop Each question
      CLI->>Dev: Ask question
      Dev-->>CLI: Answer
      CLI->>Service: Submit answer
      Service-->>CLI: Grade, feedback, next question
    end
    alt Pass threshold met
      CLI->>Git: Execute push
    else Failed or abandoned
      CLI-->>Dev: Push not run; show retry guidance
    end
```

The client should only execute the push after a successful quiz result for the same remote, refspec, and outgoing commit IDs. Recompute the outgoing range immediately before pushing; if it changed, discard the pass and quiz the new range. A quiz pass should not authorize a different push later.

## Server deployment direction

### Separate the local daemon from the hosted service

The current daemon is a useful local inference backend: it binds to loopback, auto-starts, serializes access to its in-process model, and exits after idle time. Keep that role and protocol separate from a hosted multi-user API. A hosted service needs authentication, per-user/repository boundaries, request limits, persistence policy, observability, and deployment lifecycle. Do not expose the current unauthenticated localhost endpoints to a public network.

### Suggested service boundaries

- **CLI/client:** discovers outgoing commits, collects bounded context, presents quiz, binds pass to exact commit IDs, then pushes.
- **API service:** authentication, repository/user authorization, quiz sessions, validation, rate/size limits, and policy decisions. Keep Git credentials on the client for the first version so the service cannot push on a user's behalf.
- **Inference worker:** local or hosted model runtime behind an internal job interface. Start with one serialized worker per model instance; scale workers only when model memory and queue latency are measured.
- **Persistence:** store quiz session metadata and outcome (opaque session ID, commit IDs, score, timestamps, model/prompt version, expiry). Avoid retaining raw diffs and answers by default; if retention is needed for team analytics, make it an explicit opt-in with a clear retention period.
- **Operations:** health/readiness checks, structured logs without source content, model warm-up, queue limits, timeouts, graceful shutdown, resource metrics, and a way to roll back model/prompt versions.

### Deployment stages

1. **Local prototype:** add outgoing-range collection and quiz in the existing CLI; exercise it with local inference and no hosted dependency.
2. **Service contract:** define versioned endpoints and payload limits. Keep model calls behind an interface that can target either the local daemon or remote service.
3. **Private deployment:** deploy API and one inference worker behind TLS and authentication; use synthetic/sample repositories first, then opt-in repositories.
4. **Pilot:** enable for selected users/repositories, measure latency, question rejection rate, pass/fail/retry rates, and push abandonment. Tune question count and thresholds.
5. **Broader rollout:** add organization policy controls, quotas, retention settings, and operational scaling only after pilot evidence.

The repository does not currently define a cloud provider, identity provider, or team policy. Those remain deployment choices; this document intentionally specifies service responsibilities without assuming a vendor.

## API sketch (versioned, illustrative)

```http
POST /v1/quiz-sessions
Authorization: Bearer <token>
Content-Type: application/json
```

Request includes repository identity (opaque or user-scoped), remote/ref names, base/head commit IDs, and a bounded list of commits with patches plus selected context snippets and their Git object IDs. The service should enforce limits on commit count, changed bytes, context bytes, file types, and processing time.

Response returns `session_id`, expiry, question count, policy version, and the first question. Answer submission uses `POST /v1/quiz-sessions/{id}/answers`; completion returns pass/fail and a signed or server-verifiable result tied to the exact commit IDs and refspec. This is a sketch, not a committed wire contract; finalize the contract with authentication and privacy policy before implementation.

## Failure and bypass policy

Treat the quiz gate as fail-closed by default for a configured required policy: service unavailable, invalid session, expired session, or changed outgoing commits means no push. Provide an explicit local/admin-controlled bypass for emergencies and record its reason; do not make `--force-ai` silently bypass the quiz. A local-only policy can use the local daemon, while hosted-required policy must say clearly when its service is unavailable.

Avoid an unbounded retry loop. Let users retry the quiz with a fresh session, and distinguish generation failure, grading failure, and a failed score. Teams should be able to choose thresholds, but defaults should remain short enough to avoid turning every push into a long exam.

## Security and privacy requirements

- TLS for remote service traffic and authenticated, scoped authorization.
- Bind quiz sessions to user, repository, remote/refspec, and immutable commit IDs; expire sessions and prevent replay.
- Never accept arbitrary server-side paths or commands from quiz payloads.
- Treat source code and answers as confidential. Minimize collection, logging, retention, and access.
- Redact likely credentials before transmission and reject or locally handle unsupported/binary content.
- Limit payload sizes, concurrency, generation time, answer attempts, and per-user usage.
- Protect the inference worker from prompt injection contained in source comments and documentation; repository text is untrusted input, not instructions.
- Keep push credentials on the client in the initial design.

## Success measures

- Push latency and quiz completion time (median and tail).
- Percentage of generated questions rejected by grounding/quality validation.
- Retry and abandonment rates, segmented by question count and language.
- Whether questions cover commits and important changed symbols instead of clustering on one file.
- User-reported usefulness and fairness; false failures from ambiguous grading.
- Service availability, inference queue depth, and cost/resource use per completed quiz.

## Recommended implementation sequence

1. Add a push-range abstraction that reports exactly what will be pushed, preserving per-commit diffs and handling common refspecs.
2. Add quiz data types and a local question-generator/grader interface; keep the interface independent of local daemon versus hosted HTTP transport.
3. Build semantic context from the pushed Git tree and existing RAG/parser components; attach evidence and reject ungrounded questions.
4. Implement the terminal quiz UX and bind a pass to the exact commit set. Recompute before push.
5. Persist minimal local session outcome and add opt-in policy/configuration for question count, pass threshold, and local/hosted mode.
6. Implement the authenticated hosted API and worker deployment after the local end-to-end behavior is established.
7. Pilot, measure, and tune before making the gate mandatory for all users.

## Open decisions

- Should hosted inference be the default, optional, or organization-enforced?
- What is the default question count and passing threshold?
- Should answers be free text only, or should selected questions use multiple choice for faster grading?
- How should merge commits, force pushes, multiple refspecs, and unavailable remote tracking refs be handled?
- What data, if any, may be retained for team analytics, and for how long?
- Which hosting and identity providers fit the intended deployment?

