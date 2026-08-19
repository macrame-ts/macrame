# Naming — candidates & decision log

Working notes for choosing the public name (the library is currently the
placeholder "task system", namespace `ts::`). The name will set the project
name, the GitHub org/repo, and possibly the namespace.

**DECIDED (author, 2026-08-19): Macramé** — shortlist item 0 below, exactly as
scoped there: functional spelling `macrame` everywhere, `é` branding-only;
org `macrame-ts` (registered 2026-08-19), target shape
`github.com/macrame-ts/macrame`. The namespace stays `ts::`. The rollout
checklist (what to rename, when) lives in `docs/going-public.md`; this file
and that one are deleted before the flip.

The name should: (1) encode the differentiator — *tasks declare the data they
touch; the library derives safe parallelism and a runtime harness polices it* —
not just "runs tasks fast"; (2) be findable (survive GitHub/registry/Google
search); (3) reward the reader who gets the metaphor; (4) be clean of in-domain
collisions and live software trademarks.

For a **C++ library only GitHub matters** for registration — vcpkg/Conan are
curated, PR-submitted, not name-reservable; crates.io/npm/PyPI are other
ecosystems (relevant only if we ship bindings, or for cross-ecosystem search
confusion). GitHub usernames and org names share ONE global namespace, so a
taken username blocks the matching org.

## Active shortlist (priority order, clearance-informed; sound weighted per author, 2026-07)

0. **Macramé** ("macrame" everywhere functional) — *the craft of DELIBERATE
   knotting: cords knotted into patterned, load-bearing structure.* The reclaim
   is the pitch: a tangle is what threads do without discipline; macramé is
   knots on purpose — races vs orchestrated access, in one craft word. Tagline
   candidate: "Threads, knotted on purpose." The TooManyCooks move (own the
   negative idiom as wit). **Best-sounding name on the board** (MAC-ruh-may,
   open-vowel ending, clean ASCII spelling). Clearance (full, 2026-07): GitHub
   bare `@macrame` taken (dormant user) → org variant needed (`macrame-cpp` or
   similar); crates.io taken by a dead 0.0.1 (irrelevant domain); **npm free**;
   PyPI taken by a dead 0.0.1 C/C++ build utility; co-tenant repos (Scala macro
   libs, PHP CLI kit, macramejs) none in-domain; no trademark (generic craft
   word). Caveats: first-contact knot reading must be answered by the tagline
   (README leads with it); `é` is branding-only.
   **Org variants (checked 2026-07): ALL free** — `macrame-ts`, `macrame-cpp`,
   `macrame-lib`, `macrame-tasks`, `macrame-io`; bare `macrame` is a
   zero-activity dormant user (0 repos; not reclaimable). Author preference:
   **`macrame-ts`** ("ts" = task system, echoing the `ts::` namespace). The
   earlier TypeScript-suffix caveat applies to a LIBRARY named x-ts, not to an
   org address hosting a C++ repo (one glance self-corrects); preference
   accepted. Target shape: `github.com/macrame-ts/macrame`.

1. **Entente** — *a friendly understanding / informal alliance between parties,
   and the parties collectively* (Fr. "understanding"; the Entente Cordiale).
   Names the access **agreement** between tasks about who touches what; latent
   taskforce sense (an entente is a coalition). **Cleanest clearance**:
   crates.io free; no in-domain collision; distinctive, low ambient noise.
   Co-tenants are different domains (npm = a TS linter; PyPI = a 3D-mesh lib).
   GitHub `@entente` is a dormant personal account → bare org unavailable, use
   an org variant (`entente-ts`, `entente-cpp`). Namespace `ets::` or keep `ts::`.
2. **Caucus** — *a convened working group / faction that coordinates strategy*;
   directly a **task force**, everyday (civics). GitHub org **free** (`@caucus`
   404 — the only candidate with the bare org available, a real win for the
   project URL). crates.io taken by a small recent AI-agent TUI (adjacent
   tooling, different function); npm is an empty placeholder. Namespace `cts::`
   (a wink: Clear-To-Send, the serial handshake) or `ts::`.
3. **Poset** — *partially ordered set*: a set with a reflexive/antisymmetric/
   transitive `≤` where incomparable elements model independence. The compiled
   DAG **is** a poset — mathematically the exact name for what `compile()`
   produces. No domain collision (nobody names a scheduler this), but the token
   is pre-owned by **math** packages on every registry (crates/npm/PyPI/GitHub
   user) → permanent search-sharing with combinatorics libs. Precise and
   self-documenting to the target audience; dry to outsiders. Namespace `poset::`
   (short enough to stand alone) or `pts::`.
4. **Roundabout** — *a one-way circulatory road junction; each entrant yields,
   no one stops* — the lock-free intersection vs traffic-lights-as-locks;
   everyday, warm, explains the non-blocking thesis. **Downgraded**: crates.io
   `roundabout` is already "a message-oriented concurrent runtime" — a direct
   in-domain collision (a different concurrency runtime with the exact name),
   and `rts::` reads as Real-Time Strategy to the gamedev audience. Keep only if
   the pun/everyday image outweighs the collision.

Formal USPTO TESS / EUIPO trademark pull still **pending** for the finalist —
the one clearance step not yet automated. No registered software mark (class
9/42) surfaced for any in search; all are common words or technical terms (low
registrable-mark risk, but common words are also hard to *protect*).

## Runners-up (viable, deprioritized)

- **Conch** — hold-the-conch = the exclusive right to speak (Lord of the Flies);
  a passed physical mutex token (cf. the 1889 single-track railway token). Short,
  memorable, clean (only a bare GitHub topic). Encodes exclusive-access-via-token.
- **PACT** — backronym *Parallel Access, Coordinated Tasks*; the word itself is
  the thesis (parties agree on who may touch what). Short, clean-ish.
- **Valence** — an atom's bonding capacity = a task's edges/dependencies;
  elegant, uncommon as a software name. Points at connectivity, not access.
- **Interlock** — railway interlocking makes conflicting routes physically
  impossible = `compile()` deriving safe order from conflicts; engineering
  gravitas. Some collisions; semi-generic token.
- **Maniple** — the Roman legion's independently-maneuvering tactical unit =
  coarse sub-systems composed into a frame; unique token, arcane (teaching name).
- **Baton** — conductor's baton (orchestration) + relay baton (our signature
  handoff mechanics); short. Scattered small collisions.
- **Heddle** — the loom part that selectively lifts exactly the right threads =
  a thread scheduler with access control; distinctive. Minor recent MCP-project
  collision (`goweft/heddle`).
- **MotherMayI** — the kids' game: ask permission before every move = the access
  model; TMC-spirit wit, clean (no software use). Whimsy may undersell; long token.
- **Threadwright** — coined ("one who crafts threads", cf. playwright/shipwright);
  **zero collisions anywhere**, unique, self-describing; earnest rather than witty.
- **Bouncer** — the doorman: on-the-list access or thrown out (= the harness
  fatal). TMC-grade wit, gamedev-compatible; reads informal.
- **Vetinari** — Discworld's Patrician, the arbiter who makes a chaotic city work
  by resolving every conflict invisibly. Strong *story*, narrow (Pratchett) audience.
- **Escapement** — the clock mechanism converting stored energy into regulated,
  discrete, timed releases = a scheduler in a precision-craft term. Clean; encodes
  timing/dispatch rather than access. (User: "doesn't sound good.")
- **Clearance** — the ATC/security *grant* to use a shared contested resource
  (runway) = the access grant, everyday. Common word (findability).
- **Zipper** — the zipper merge: fair alternating access to one shared lane =
  fair interleaving of contending streams. Some FP-"zipper" collision.
- **CAST** — *Concurrent Access-Scheduled Tasks* / a theatrical cast (a company
  with defined roles acting in concert); rich double meaning, very common token.

## Rejected (collision or poor fit)

- **Weave / Weaver** — fatal: [Weave](https://github.com/mratsim/weave) is a Nim
  work-stealing multithreading runtime (our exact category); the textile shelf is
  colonized *by concurrency projects* (**Loom** = Tokio's tester + Java virtual
  threads; **Shuttle** = AWS concurrency tester; **Tapestry**, **Warp**,
  **Arachne**, **Jacquard** all taken/in-domain). The metaphor is also generic —
  every DAG scheduler "weaves tasks".
- **Interwoven** (proposed 2026-07) — fatal: Interwoven, Inc. was a major
  NASDAQ-listed software company (TeamSite CMS; Autonomy 2009 → HP 2011 →
  OpenText 2016), and the trademarks LIVE ON under OpenText — a held
  software-class mark plus two decades of search residue. Also the colonized
  textile shelf (adjacent to Weave), and a past-participle adjective reads
  awkwardly as a product noun. Merit acknowledged: names the *result* (strands
  made into one safe fabric) better than "Weaver" — that aspect's cleared
  survivor is Heddle.
- **Interwoven-ts** (proposed 2026-07) — fatal twice: a suffix does not defeat
  same-class mark similarity, and Interwoven's flagship WAS "Interwoven
  TeamSite" = "Interwoven TS" (`iw`/`iwts`-prefixed tooling) — the suffix walks
  into the historical product's own nickname. Independently, `-ts` on GitHub
  reads as "TypeScript port of" — wrong signal for a C++ library. CAVEAT
  recorded: the defensively reserved `-ts` orgs (roundabout-ts etc.) are fine as
  parking, but a finalist's real org should be `-cpp` or the full form, not `-ts`.
- ~~Macramé — initially rejected on the knot metaphor~~ **REVERSED, promoted to
  the shortlist** (author pushback, 2026-07: sound is a first-class criterion).
  See the shortlist entry.
- **Arbiter** — *the* hardware term (a bus arbiter grants memory access = this
  library) but taken: a Rust multi-agent framework + crates.io + several others.
- **Cardea** — Roman goddess of the door-hinge ("opens the shut, shuts the open"
  = a reader/writer lock); taken (healthcare AutoML, Hyperledger credentials).
- **Rubato** — musical "stolen time" = work-stealing; taken (a known Rust audio crate).
- **Fugue** — interweaving independent voices → coherent whole; taken (Fugue
  distributed computing, Atlassian Fugue, a binary-analysis Fugue).
- **Muster** — "assemble a task force"; taken (Facebook batching lib, LLNL, Giant Swarm).
- **Cadre** — a small core group; taken (Modal Labs config service, ML frameworks).
- **Quorum** — semantically colonized ("distributed consensus": JPMorgan/ConsenSys/Certora).
- **Turnstile** — taken in-domain (a Go concurrency manager / rate limiter).
- **Pauli** — exclusion principle = mutual exclusion (elegant), but crowded in quantum SDKs.
- **Concord / Cadence / Chord** — Concord crowded (jet/place/grape); Cadence =
  Cadence Design Systems (large EDA trademark); Chord = the famous DHT.

## Namespace options

- Keep **`ts::`** ("task system") regardless of the marketing name — zero churn,
  already in use, decouples name from code. The name then lives only in the repo/docs.
- Or a compound "**<Name> Task System**" → 3-letter namespace: `ets::` (Entente),
  `cts::` (Caucus), `pts::`/`poset::` (Poset), `rts::` (Roundabout — clashes with RTS).
  Only worth it if you want the name visible at every call site (one-time rename).

## Registration plan (defensive, do at decision time)

- **GitHub only** for a C++ lib. Reserve the org (free, acceptable): the `-ts`
  variant orgs are free; bare `entente`/`poset`/`roundabout` orgs are blocked by
  dormant users; bare `caucus` org is free.
- crates.io/npm/PyPI: skip unless shipping bindings. crates.io is **immutable**
  (a placeholder is permanent) — don't strand dead crates for dropped candidates;
  at most claim the one name we'll ship.
- Name reclamation reality: inactivity ≠ reclaimable. GitHub releases names only
  on trademark claim (no inactive-name process); crates.io names are permanent;
  PyPI has PEP 541 (abandoned-project transfer); npm has a disputes process.

---

# Going-public track — state & open threads (to continue later)

The overarching goal driving this session: make the library public on GitHub —
coherent state, docs, hygiene. Status of that track:

**Done**
- LICENSE — MIT (`4b2a8dd`).
- Namespace coherence — the whole scheduler layer moved into `ts::`/`ts::detail`
  (was global): `Scheduler`, `Scheduler_config`, `Idle_policy`, `Priority`, the
  queue/deque/eventcount internals (agent A2, `a81b752`; 340/0 + TSan clean).
- Public docs — `docs/guide.md` (user guide, mid-level MT-literate) and
  `docs/design.md` (rationale + a 10-entry rejected-alternatives section) written;
  freshness sweep fixed stale TODO items (coroutines were mislabeled SHELVED —
  they shipped) and old option-type names (agent A1, `ed519b1`). Public-docs
  contract recorded in CLAUDE.md (update guide.md/design.md every public-API push).
- CMake — `CMakeLists.txt` + `CMakePresets.json` alongside the vcxproj; presets
  windows-msvc / windows-clang-cl / linux-clang / linux-tsan; Windows build
  verified 340/0 (agent A2, `8f29594`). Linux presets transcribed from the green
  `tsan/run.sh` but unverified (WSL lacks cmake).

**Pending — blocked on the name**
- README.md — full draft written and reviewed in-session (thesis-first pitch,
  30-sec Guarded+graph example, verification section, WIP/status, build/docs
  links). Embeds the project name, so parked until the name is chosen. Open calls
  at draft time: display name, keep the UE-lineage sentence in the opener?,
  perf numbers in/out (left out as WIP).

**Pending — name-independent (safe to do now)**
- CI (GitHub Actions) — none exists (`.github/` absent). Want: Windows
  MSBuild+`--tests`, Linux clang build+`--tests`, a **TSan** job (the suite is
  portable — the differentiator for a concurrency lib), an ASan job. Unverifiable
  until first push to GitHub.
- Repo hygiene from A2's audit:
  - Untrack personal launcher scripts `claude_new.bat` / `claude_resume.bat`
    (and reconsider tracking `.claude/settings.json`) — don't ship personal tooling.
  - Add a `ts.h` umbrella header including the public surface (guarded / task /
    static_task_graph / parallel_for / deferred / versioned / coroutine_support /
    scheduler) so users need one include.
  - Public/internal header split: `event_count.h`, `mpmc_queue.h`,
    `work_stealing_deque.h`, `worker_thread.h` are pure internals sitting next to
    the public API — consider a `detail/` subfolder (low churn) vs a full
    `include/ts/` layout (conventional, heavy churn; defer while exe-driven).
  - `.clang-format` — A2 proposed one encoding the house style (Snake_case types,
    4-space, Allman incl. lambdas, ~100 col, no alignment padding); adding it is
    safe (doesn't reformat until run), full reformat is a separate call.
  - `main.cpp` — add `--help` for the `--tests/--bench/--stress/--memprofile/--death` CLI.
  - `design/guarded_sketch.h` — pre-rename historical sketch; keep as history
    (referenced from CLAUDE.md) or delete. Author's call.

**Roadmap** — the full technical roadmap lives in `docs/TODO.md`; going-public
work is the P0 track layered on top. Post-public technical priorities (unchanged):
deferred/versioned main chain (journal→arena→record-stream→typed tier), the
postponed allocation items, ambient scheduler, entity-interaction primitives.
