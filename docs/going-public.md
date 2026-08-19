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
- [ ] **Comprehensive code review** — whole codebase, concurrency-critical paths first
      (scheduler, pipe, task block, graph acquire/handoff, the recent `access`/`async` + MSVC fixes).
- [ ] **Docs review** — all docs *except* README (accuracy, coherence, currency after this session's churn).
- [ ] **API-stability pass** — resolve the pre-public API inconsistencies (see `docs/TODO.md`
      → "Inconsistencies to resolve"): `run_inline`-on-`access`, multi-object `access` semantics,
      stale `async`→`access` comments, `pipe`/`queue` terminology. Lock the public surface while
      still pre-1.0 — breaking changes after public are costly.
- [x] **Finalize the library name** — decided 2026-08-19: **Macrame** (`macrame`
      functional, `é` branding-only; org `macrame-ts`, target `github.com/macrame-ts/macrame`;
      see `docs/naming.md`).
- [ ] **Name rollout** (repo-local, before or at the move):
      - `--version` product string in `src/main.cpp` (`task_system %s` → `macrame %s`);
        `version.h` macros stay `TS_*` (namespace decided, below).
      - [x] Namespace decided (author, 2026-08-19): **stays `ts::`** (the org suffix echoes it).
      - Decide whether project/file names (`task_system.slnx`, `task_system.vcxproj`,
        CMake `project(task_system)`, the built exe, `tsan/run.sh`, CI yml, docs that
        spell `task_system --trace`) rename to `macrame` now or stay as working names.
      - `README.md` header comment + `CONTRIBUTING.md` note already point at the move.
- [ ] **GitHub side** (author): ~~create the `macrame-ts` org~~ — registered (2026-08-19).
      Remaining: transfer `Andriy06/task_system` → `macrame-ts/macrame` (a transfer keeps
      redirects; do it BEFORE the flip so badges/links are final), set the repo
      description + topics, refresh the README CI badge owner/repo.
- [ ] **Re-measure `docs/example-frame-optimization.md`** — its figures predate the
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
- [ ] **Confirm CI green** on the final repo.
- [ ] **Decide whether `CLAUDE.md` ships** — it is tracked, so it will appear in the public
      repo. It is the internal, LLM-oriented dev doc; common to ship, but a conscious call.
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
