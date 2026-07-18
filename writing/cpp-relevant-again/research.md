# Research: Why C++ Will Be Relevant Again (C++26)

## Key Verified Facts & Sources

### Timeline / ratification
- ISO C++ committee finalized C++26 on **March 28–29, 2026, in London (Croydon), UK**, after a 6-day meeting.
- ~210 experts (130 in-person, 80 remote), 24 nations; 411 national-body comments addressed. [1][3]
- Feature-complete after June 2025 Sofia meeting; final ballot March 23–28, 2026. [4]
- GCC 16.1 (May 8, 2026) already supports most C++26 features. [4]
- Next meetings: June 2026 (Brno), Nov 2026 (Búzios, Brazil). [1]

### Reflection (P2996)
- Compile-time introspection via the `^^` ("cat-ears") operator — static, NOT runtime like Java/C#. [4]
- Herb Sutter: "the biggest upgrade for C++ development ... since the invention of templates"; the language's "decade-defining rocket engine." [1]
- Lets C++ "describe itself and generate more" at compile time, zero runtime overhead. [2]
- Enables user-defined abstractions like `interface`, `copyable`, `ordered`, `union` compiling to standard ISO C++. [2]
- New `<meta>` header. [4]

### Memory safety / hardening
- Hardened standard library: bounds safety for vector, span, string, string_view, etc. [2][3]
- Reading uninitialized local variables is no longer undefined behavior (erroneous behavior model); `[[indeterminate]]` attribute to opt out. [4]
- **Google production results (via Sutter):** hardened libc++ across hundreds of millions of LOC — fixed **1,000+ bugs**, projected to prevent **1,000–2,000 bugs/year**, **30% reduction in segfault rate** across production fleet, avg perf overhead **~0.30%**. Only **7 instances** needed opt-out. Achieved by recompiling. [1][2][6]
- Deployed at Apple and Google platforms. [1]
- Stroustrup's Profiles (P3274): portable, tool-supported safety profiles. [6]

### Contracts (P2900 / P3846)
- Pre-conditions (`pre`), post-conditions (`post`), and `contract_assert`. [1][4]
- Four violation-handling modes: ignore, observe, enforce, quick-enforce. [2]
- Visible to callers and static-analysis tools; replaces C's `assert` macro. [2]
- Final committee vote: 114 for, 12 against, 3 abstain. [1]

### std::execution — senders/receivers (P2300)
- Unified async/concurrency framework: schedulers, senders, receivers. [1][2]
- Integrates with C++20 coroutines; "structured concurrency," "data-race-free by construction." [1][2]
- `std::execution::task<T, Env>`, parallel scheduler in `<execution>`. [4]

### Other notable library additions [4]
- `<simd>` — data-parallel SIMD
- `<linalg>` — BLAS-based linear algebra
- `<hive>`, `<inplace_vector>`, `<hazard_pointer>`, `<rcu>`, `<debugging>`, `<text_encoding>`
- `std::optional<T&>`, `std::saturating_add/…`, `std::views::concat`, `std::copyable_function`
- `#embed` (from C23)
- constexpr: exceptions, placement new, containers, virtual inheritance
- `template for` expansion statements, pack indexing

## Context: why C++ "declined" (background, general knowledge — mark as analysis not cited fact)
- Managed languages (Java/C#) traded some perf for GC safety and productivity.
- Web/mobile growth had no natural C++ home (JS, Swift, Kotlin).
- Python won productivity/data-science/ML.
- Memory-safety reckoning: Microsoft & Google both reported ~70% of serious CVEs are memory-safety issues; NSA/CISA/White House ONCD memos urged moving away from memory-unsafe languages; Rust rose as the safe-and-fast alternative.
- C++ complexity: slow compiles, template errors, build tooling (CMake).

## Sources
[1] Herb Sutter, "C++26 is done! — Trip report: March 2026 ISO C++ standards meeting" — https://herbsutter.com/2026/03/29/c26-is-done-trip-report-march-2026-iso-c-standards-meeting-london-croydon-uk/
[2] InfoQ, "C++26: Reflection, Memory Safety, Contracts, and a New Async Model" (2026) — https://www.infoq.com/news/2026/04/cpp-26-reflection-safety-async/
[3] InfoQ, "C++26 Draft Finalized with Static Reflection, Contracts, and Sender/Receiver Types" (2025) — https://www.infoq.com/news/2025/06/cpp-26-feature-complete/
[4] Wikipedia, "C++26" — https://en.wikipedia.org/wiki/C++26
[5] Sandor Dargo, "C++26: Standard library hardening" — https://www.sandordargo.com/blog/2026/05/13/cpp26-library-hardening
[6] John Farrier, "C++26 Memory Safety Is the First Serious Answer to the Rewrite Fantasy" — https://johnfarrier.com/c26-memory-safety-is-the-first-serious-answer-to-the-rewrite-fantasy/
