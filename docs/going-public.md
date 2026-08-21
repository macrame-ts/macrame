<!-- Internal planning checklist for the public launch. DECIDED (2026-08-19): this file and
     docs/naming.md are deleted before the flip (see the checklist item below).
     The name decision lives in naming.md; this tracks everything else. -->

# Going-public checklist

Living list of what must happen to make the repository public. Check items off as
they land. **Re-run the secrets scrub (below) immediately before flipping.**

## Done (prep already merged)

- [x] MIT `LICENSE`.
- [x] Whole library in namespace `ts::` / `ts::detail` (scheduler layer moved in).
- [x] Public docs: `README.md` (root), `docs/quickstart.md`, `docs/guide.md`,
      `docs/design.md`, `docs/task-systems-comparison.md`; public-docs contract in `CLAUDE.md`.
- [x] CMake build + presets (`windows-msvc`, `windows-clang-cl`, `linux-clang`, `linux-tsan`)
      alongside the VS solution.
- [x] CI (GitHub Actions): Windows/MSVC tests, Windows/clang-cl tests, Linux/TSan — green.
- [x] Hygiene: `ts.h` umbrella header, `.clang-format`, `--help` / `--version`; personal
      launcher scripts and `.claude/` untracked.
- [x] MSVC clean-build fix (the deferred/versioned samples now compile under `cl`, not only clang-cl).
- [x] `access` / `async` API split.
- [x] Shipping build config (harness compiled out via `TS_SAFETY_CHECKS=0`) + version `0.1.0`
      + `CONTRIBUTING.md` + issue/PR templates.
- [x] Secrets/paths scrub (clean); personal `/mnt/c/...` paths genericized in `tsan/`.

## Before the flip

- [x] **Roadmap tidy + triage** — done; see `docs/TODO.md` (scannable index + pre-public candidates).
- [x] **Comprehensive code review** — DONE (2026-08, this session): C1–C5 + Access_op re-review + Deferred/Versioned/journal + Event_bus + coloring + Frame_gate; every finding applied or consciously deferred. Original scope: whole codebase, concurrency-critical paths first
      (scheduler, pipe, task block, graph acquire/handoff, the recent `access`/`async` + MSVC fixes).
- [ ] **Docs review** — all docs *except* README (accuracy, coherence, currency after this session's churn).
- [ ] **API-stability pass** — resolve the pre-public API inconsistencies (see `docs/TODO.md`
      → "Inconsistencies to resolve"): `run_inline`-on-`access`, multi-object `access` semantics,
      stale `async`→`access` comments, `pipe`/`queue` terminology. Lock the public surface while
      still pre-1.0 — breaking changes after public are costly.
- [x] **Finalize the library name** — decided 2026-08-19: **Macrame** (`macrame`
      functional, `é` branding-only; org `macrame-ts`, target `github.com/macrame-ts/macrame`;
      see `docs/naming.md`).
- [x] **Name rollout** — DONE (`35b1288`, pushed). (repo-local; 73 `task_system` occurrences across 19 tracked
      files + 3 tracked files to rename, scoped 2026-08-20). Namespace **stays `ts::`**
      (author, 2026-08-19), so `include/ts/`, `TS_*` macros and `version.h` are UNCHANGED.
      Groups A-E are one local, CI-testable batch; do them together (a missed CI/exe path
      turns CI red). Verify: `cmake --preset windows-msvc` build → run `macrame.exe --tests`
      → `git grep task_system` returns only intended history (CHANGELOG) → push → 4 CI jobs green.
      - **A. Physical renames** (`git mv`): `task_system.slnx` → `macrame.slnx` (fix its
        internal `<Project Path="task_system.vcxproj">` → `macrame.vcxproj`),
        `task_system.vcxproj` → `macrame.vcxproj`, `task_system.vcxproj.filters` →
        `macrame.vcxproj.filters`. **Keep the vcxproj Project GUID** (`8acc9866-...`) so the
        local `.args.json` binding survives.
      - **B. Build identity**: `CMakeLists.txt` `project(macrame)` + `add_executable(macrame)`
        + every `target_*(macrame)` + comments (output exe auto-becomes `macrame.exe`);
        `macrame.vcxproj` `<RootNamespace>` `tasksystem` → `macrame` (cosmetic).
      - **C. Code strings**: `src/main.cpp` `--version`/usage banner `task_system` → `macrame`;
        `src/fatal.cpp` `task_system_crash.dmp` → `macrame_crash.dmp`.
      - **D. CI + scripts** (must move with B): `.github/workflows/ci.yml` all
        `.../task_system.exe`/`.pdb` → `macrame.*`, artifact `task_system_crash.dmp` →
        `macrame_crash.dmp`, log strings (all 4 jobs); `show_graph.bat`, `tsan/run.sh`,
        `tsan/README.md` command/path refs.
      - **E. Docs** (user-facing command name `task_system --X` → `macrame --X`, `.slnx` name):
        `README.md`, `guide.md`, `quickstart.md`, `CONTRIBUTING.md`, `example-frame-optimization.md`,
        `.github` issue/PR templates, and internal `CLAUDE.md` (VS Project section) /
        `graph-viz-handoff.md` / this file.
      - **Preserve local VS config** (untracked, plain `mv` not `git mv`, content unchanged):
        `task_system.vcxproj.user` → `macrame.vcxproj.user` and `task_system.args.json` →
        `macrame.args.json` (holds the `--tests` debug launch args + history; VS keys `.user`
        by the vcxproj basename, so a rename without these silently loses the config). The
        generic `.gitignore` patterns (`*.user`, `*.args.json`) already cover the new names.
      - **Cleanup leftovers** (all untracked/ignored, local): delete the `task_system/` build
        dir, `task_system_crash.dmp`, and `build/windows-msvc/` (stale `task_system.exe`);
        reconfigure CMake clean so no old-named artifact lingers beside `macrame.exe`.
      - `README.md` header comment + `CONTRIBUTING.md` note already point at the move.
      - **Exe-name note**: the library-extraction item below claims the `macrame` name for the
        *library* target; the driver exe then needs a distinct name (e.g. `macrame_driver`).
        Sequence the extraction and this rename together, or the rename's "exe becomes
        `macrame.exe`" collides with the library target.
- [x] **Extract the library from the driver executable** (going-public structure). DONE
      (2026-08-20): static library `macrame` (`macrame::macrame`) split from the driver exe,
      now `macrame_playground`. CMake: `add_library(macrame STATIC ${TS_CORE_SOURCES})` +
      install/export + generated `macrame-config.cmake` so `find_package(macrame CONFIG)` works;
      config defs (`TS_SAFETY_CHECKS`/`TS_PROFILING`/`_HAS_EXCEPTIONS=0`) are PUBLIC for
      ODR-consistency; `tests/consumer/` smoke-test + a `windows-consumer` CI job prove
      find_package from outside the tree. VS: `macrame.slnx` split into the `macrame` static-lib
      project and the `macrame_playground` driver project (ProjectReference). Static only - no
      dynamic library (author, 2026-08-20). Verified: CMake lib+playground build, playground
      --tests 749/0, consumer runs; VS both projects build. Deferred detail below kept for history.
      Today one monolithic target compiles the 6 `TS_CORE_SOURCES` + headers *and* the whole
      driver (`main.cpp`, samples, benchmarks, tests) into one exe (`CMakeLists.txt`); a
      consumer cannot link the library without building the test harness. Split into:
      - **A library target** — `add_library(macrame STATIC ${TS_CORE_SOURCES})` (the 6 `src/*.cpp`
        + `include/ts`), alias `macrame::macrame`, with
        `target_include_directories(macrame PUBLIC $<BUILD_INTERFACE:...include> $<INSTALL_INTERFACE:include>)`
        and `target_compile_features(macrame PUBLIC cxx_std_23)`. The library is a compiled
        static lib, not header-only (real implementation in scheduler/pipe/graph `.cpp`).
      - **A driver executable** — `main.cpp` + samples + benchmarks + tests, linking the
        library. Needs a name distinct from the `macrame` library target (`macrame_driver` /
        `macrame_tests`); it is a dev/test artifact, not shipped.
      - **Config-consistency (load-bearing)**: `TS_SAFETY_CHECKS` / `TS_PROFILING` must be
        **INTERFACE/PUBLIC** compile definitions on the library so a consumer compiles its own
        TUs with the same values the library was built with — mixed config is an ODR violation,
        and the `access.h` link-time tripwire (`detect_mismatch`) will fire (CLAUDE.md code-style
        note). Decide the default-config story a consumer inherits.
      - **Consumability (decide depth)**: minimal = `add_subdirectory()` works
        (`target_link_libraries(app macrame::macrame)`); fuller = `install(TARGETS ... EXPORT)`
        + `install(DIRECTORY include/ts ...)` + a generated `macrame-config.cmake` so
        `find_package(macrame)` works, then optionally vcpkg/Conan later. Near-term: at least the
        target split + `add_subdirectory`; the export/find_package layer can trail public.
      - **Static vs shared** (recommendation, 2026-08-20): **static by default; no supported
        Windows DLL for 1.0.** The API is template-heavy with no stable ABI (consumers
        instantiate `Guarded<T>`/`Task<R>`/`Access_op` and bake `Task_control_block`'s layout
        into their own binary), the compiled core is tiny (6 `.cpp`), and `TS_SAFETY_CHECKS`
        config must match — so a DLL delivers little and costs export annotations + an ABI
        promise the pre-1.0 API can't make. Keep the door open cheaply: `add_library(macrame ...)`
        without hardcoding `STATIC` (respects `-DBUILD_SHARED_LIBS=ON`) + `POSITION_INDEPENDENT_CODE ON`;
        document shared as experimental (Linux/macOS only) until there's demand and a stable-ABI
        story. The one case a single shared `.dll` is the *right* answer is a plugin host where
        host + plugins must share one scheduler and one TLS universe (see the scheduler-ownership
        item) - revisit if that becomes a target.
      - **VS solution mirror**: split `macrame.vcxproj` into a library project + a driver
        project (or keep CMake as the packaging authority and the single vcxproj as the driver).
      - **CI**: build both targets; add a tiny consumer smoke-build against the exported/added
        library so a broken include-interface or export set fails CI, not a downstream user.
      - Keep the three-list sync rule (`.vcxproj` / `.filters` / `CMakeLists`) across the split.
- [x] **Decide: keep the process-global scheduler, or give the user ownership of its lifetime.**
      DONE (2026-08-20): chose option 2, user-owned single instance, via explicit subsystem-init
      functions rather than a held handle (the user never uses a Scheduler instance, so a handle
      is ceremony + an accidental-early-teardown footgun). New API: `ts::create_scheduler(config)`
      / `ts::destroy_scheduler()` (SDL_Init/SDL_Quit shape), `scheduler_running()`; no lazy
      construction (a scheduler is heavy - `global_scheduler()` fatals if none); exactly one per
      process (a second `create_scheduler` is fatal); the private ctor + `detail::make_scheduler`
      stay, the holder enforces the singleton; `Scheduler_scope` reframed to create/reconfigure;
      `configure_scheduler` retired. Clean-shutdown safety net at program exit. 3 death tests
      (double-create, use/destroy-when-none). Suite 749/0; guide/design/quickstart updated.
      *(Original analysis retained below for context.)*
      An API-shape call best locked pre-1.0 (cheap now, breaking later). Today there is exactly
      one process-wide instance behind `global_scheduler()`, spun up lazily on first use; the
      `Scheduler` ctor is factory-gated (private + `detail::make_scheduler`), so ad-hoc
      construction does not compile, and reconfiguration is teardown+recreate
      (`configure_scheduler` / `Scheduler_scope`). This was a deliberate 2026-07/08 hardening:
      the single-pool invariant is what let `current_scheduler` (the thread_local "which
      scheduler" selector) be retired, `execute()` drop its `Scheduler&` param, and every call
      site resolve `global_scheduler()` directly.
      - **Why it matters for public/embedders**: a library (post-extraction) with a *hidden*
        process-global singleton that lazily spawns threads is awkward for embedding - no
        control over startup/shutdown ordering, DLL-unload teardown, or test isolation, and
        surprising for a host that wants to own thread lifetime. Public users will hit this.
      - **Option ladder** (increasing cost/blast radius):
        1. **Status quo** - hidden global + `configure_scheduler`/`Scheduler_scope`. Zero work;
           keeps the clean single-pool internals.
        2. **User-owned single instance** - the user constructs one `Scheduler` at startup and
           installs it (RAII/handle), still exactly one live at a time. Restores explicit
           lifetime control (init order, shutdown, embedding, tests) *without* reintroducing the
           "which scheduler" question or `current_scheduler`. Middle ground; ungates the ctor
           but keeps the single-pool invariant.
        3. **Full multi-scheduler / ambient selection** - multiple live pools, per-call routing
           (`launch`/`async`/`execute`). Reintroduces `current_scheduler` and threads a target
           through the API the single-global explicitly removed. This is the roadmap's
           exploratory item (theme 2 / guide §13), demand-gated - the plugin-host shared-`.dll`
           case (extraction item) is one motivation.
      - **Recommendation to weigh**: option 2 is the likely sweet spot for a public library -
        it removes the embedder-hostile hidden global and the surprising lazy thread spawn while
        preserving the single-pool simplicity that made the rest of the design tractable; option
        3 stays a post-1.0, demand-gated exploration. Cross-refs: the library-extraction item
        (embeddability) and roadmap theme 2.
- [ ] **GitHub side** (author): ~~create the `macrame-ts` org~~ — registered (2026-08-19).
      Remaining: transfer `Andriy06/task_system` → `macrame-ts/macrame` (a transfer keeps
      redirects; do it BEFORE the flip so badges/links are final), set the repo
      description + topics, refresh the README CI badge owner/repo.
- [x] **Re-measure `docs/example-frame-optimization.md`** — DONE (`c8aa228`, the 60 FPS figures). Its figures predated the
      `parallel_for` current-scheduler routing fix (see the note at the top of that doc):
      re-run baseline/optimised makespan, utilization, and dead time on the 6-worker trace,
      interleaved A/B with medians (the measurement rule), and update the doc's numbers
      before publishing. The qualitative analysis and the levers stand. While at it,
      regenerate `docs/media/game_frame_trace.svg` (the README embed) from the fresh trace.
- [ ] **Re-run the secrets/paths scrub** (must be clean):
      ```
      git grep -niE "c:\\\\users\\\\[a-z]|/home/[a-z]+/|/mnt/c/" -- .            # personal paths
      git grep -niE "ghp_[A-Za-z0-9]{20}|xox[baprs]-|AKIA[0-9A-Z]{16}|-----BEGIN" -- .   # credentials
      git grep -niE "[a-z0-9._%+-]+@(gmail|outlook|yahoo|hotmail)\." -- . | grep -v LICENSE  # emails
      ```
- [ ] **Badges** in `README.md`: add/enable the CI-status and license badges with the FINAL
      owner/repo in the URL (the CI badge URL embeds `owner/repo`, so it must be refreshed if
      the repo is renamed or moved to an org).
- [x] **Clean-clone build/run smoke test** — DONE (2026-08-20): fresh `git clone` of origin
      (committed files only, no `.user`/build dir) built and ran on every path -- CMake
      `windows-msvc` (`macrame_playground --tests` 749/0) + `windows-shipping`; the VS path via
      MSBuild of both `.vcxproj` (macrame.lib + macrame_playground.exe through the
      ProjectReference); and the consumer path (install -> `find_package(macrame)` -> build ->
      run, counter=42 launch=42). No untracked-state reliance, no vcxproj/CMake drift. (Re-run on
      a second machine / fresh profile stays ideal before the flip.) Original notes below.
      (final local gate; run AFTER the rename and the
      library extraction, since both change the build). Prove a fresh checkout builds and runs
      from committed files alone — the catch is anything that works locally only because of
      untracked/uncommitted state (build artifacts, the `.vcxproj.user`, a source in the
      `.vcxproj` but missing from `CMakeLists.txt` — the documented CI-break class — or a
      hardcoded local path).
      - Get a pristine tree with **no local state**: clone to a scratch dir
        (`git clone . /tmp/macrame-clean` or clone the remote), or `git archive HEAD | tar -x`
        into an empty dir. Do NOT copy the working tree (it carries `build/`, `.user`, dumps).
      - **CMake path** (CI's authority): each preset configures + builds clean —
        `windows-msvc`, `windows-clang-cl`, `windows-shipping`, `linux-clang`; then run the
        driver: `--tests` (exit 0), `--bench`, `--stress`, `--dot`, `--trace`. Shipping runs
        `--tests` too (harness-dependent cases report skipped).
      - **VS path** (what a VS user does): open `macrame.slnx` on a machine/dir with no prior
        build output, build x64 Release + Shipping, run the driver. Catches vcxproj-only drift
        that CMake would not.
      - **Consumer path** (once the library is extracted): a throwaway project does
        `add_subdirectory` (and, if the export layer landed, `find_package(macrame)`) and links
        `macrame::macrame` against a 5-line `co_await`/`Guarded` program — proves the public
        include interface and any export set work from outside the tree.
      - Ideally on a **second machine / fresh user profile** (no VS caches, no `%TEMP%` state)
        to catch machine-local assumptions; at minimum a clean directory.
- [ ] **Confirm CI green** on the final repo.
- [x] **Decide whether `CLAUDE.md` ships** — YES, it ships (author, 2026-08-20). It stays
      tracked and appears in the public repo (the internal, LLM-oriented dev doc).
- [ ] **Remove the internal planning docs before the flip** (decided, author, 2026-08-19):
      delete this file and `docs/naming.md`, and scrub the references to them — the
      `README.md` header comment and the CI-badge NOTE comment point here, so drop or
      reword those at the same time (`git grep -l "naming.md\|going-public.md"` to catch
      any others).

## Flip mechanics (fast, once the name is set)

- [ ] Make the repository public.
- [ ] Set the GitHub description + topics (discoverability).
- [ ] Confirm Actions runs on the public repo and the README badges render for anonymous viewers.
- [ ] Optional: branch protection on `master` (require CI green to merge).
- [ ] **Enable GitHub Pages** on `macrame-ts/macrame` (Settings -> Pages -> deploy
      `/docs` from `master`) and verify the README's "interactive version" link goes
      live. The README already points at the final Pages URL
      (`macrame-ts.github.io/macrame/media/game_frame_trace.svg`, written as-if-done
      2026-08-19; a NOTE comment sits next to it) - the link is DEAD until this step.
      Background: GitHub cannot serve the scripted SVG itself (the blob page sanitizes
      to a static image; even raw is served with `CSP: sandbox`, verified 2026-08-19);
      Pages is the self-owned non-sandboxed host and later also hosts a docs site.
      Zero-setup fallback if Pages is dropped: jsDelivr
      (`cdn.jsdelivr.net/gh/macrame-ts/macrame@<ref>/docs/media/...`, third-party;
      pin a tag/commit ref - `@master` is CDN-cached for ~12 h).
- [ ] Tag `v0.1.0` and cut a GitHub release. (Nothing was ever tagged, so the library
      goes public as 0.1.0 — the coroutine-first and pipe transformations are part of
      what 0.1.0 *is*, not a delta against a released predecessor. Author decision,
      2026-08. See [CHANGELOG.md](../CHANGELOG.md).)

## Trailing (fine to do in the open)

- [ ] Benchmark regression baseline (postponed — not blocking).
- [ ] Announcement, if desired.
