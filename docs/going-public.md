<!-- Internal planning checklist for the public launch. Delete (or archive) at the flip.
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
- [ ] **Finalize the library name** (see `docs/naming.md`). Then update: `README.md` title
      + the placeholder header comment, `LICENSE` if needed, the `TS_VERSION`/`version.h`
      product string and any `--version` text, the namespace if it changes, and the GitHub
      org/repo.
- [ ] **Re-measure `docs/example-frame-optimization.md`** — its figures predate the
      `parallel_for` current-scheduler routing fix (see the note at the top of that doc):
      re-run baseline/optimised makespan, utilization, and dead time on the 6-worker trace,
      interleaved A/B with medians (the measurement rule), and update the doc's numbers
      before publishing. The qualitative analysis and the levers stand.
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
- [ ] **Decide the fate of the internal planning docs** — this file and `docs/naming.md`:
      keep, archive, or delete at the flip.

## Flip mechanics (fast, once the name is set)

- [ ] Make the repository public.
- [ ] Set the GitHub description + topics (discoverability).
- [ ] Confirm Actions runs on the public repo and the README badges render for anonymous viewers.
- [ ] Optional: branch protection on `master` (require CI green to merge).
- [ ] Tag `v0.1.0` and cut a GitHub release. (Nothing was ever tagged, so the library
      goes public as 0.1.0 — the coroutine-first and pipe transformations are part of
      what 0.1.0 *is*, not a delta against a released predecessor. Author decision,
      2026-08. See [CHANGELOG.md](../CHANGELOG.md).)

## Trailing (fine to do in the open)

- [ ] Benchmark regression baseline (postponed — not blocking).
- [ ] Announcement, if desired.
