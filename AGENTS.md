# Fork maintenance

- The maintained branch is `multi` and its public name is Multi. The repository remains `hjqcan/LASRecomp`, with `main` as the default. Keep legacy `enhanced-beta*` release tags and internal filenames/data paths compatible as documented in `docs/multi.md`.

- `main` and `beta_5` mirror the corresponding `peiikai/LASRecomp` branches. Update them only by fast-forwarding to upstream. Do not put fork features on either branch.
- `multi` is the maintained fork edition. Merge upstream `beta_5` updates into it; resolve conflicts by preserving the enhancements listed in `docs/multi.md` alongside upstream fixes.
- Develop and commit ongoing work, including Z-A research, directly on `multi` by default. Merge official updates there too. Use temporary branches only when isolation is actually needed or for focused upstream PRs, and delete completed integration branches once their commits are on remote `multi`.
- Do not reset `multi` to upstream, discard features rejected by upstream, or overwrite feature branches backing open PRs. Upstream submissions remain small, independent PRs against `beta_5` and follow the maintainer's scope requirements.
- Keep archive import (NSP/NSZ/XCI/XCZ), translated UI, version slots and experimental updated-version randomizer support in `multi`. They must stay out of upstream PRs unless upstream explicitly changes its policy.
- Multi-game work also belongs on `multi`; follow `docs/multi-game.md`. Keep common runtime fixes reusable, gate title-specific behavior by adapter, and preserve existing LAS behavior and saves. A catalog entry or successful compilation alone must not mark a new game playable.
- Source integration and release qualification are separate: unfinished ports may be checked into `multi` with explicit development gates and documented failures. They do not need to remain on unpublished branches until playable. Keep prerelease acceptance checks in place.
- Verify the Windows package and the regression checklist in `docs/multi.md` before publishing a fork prerelease. Do not package game assets, generated game code, keys, saves, seeds or personal settings.
- Keep documentation focused on current usage, architecture and limits. Do not commit dated validation diaries, per-run reports or machine-specific evidence.
- Prefer focused tests for changed behavior and reuse existing CI jobs. Avoid duplicate smoke scripts, source-text assertions and repeated test runs that add no coverage.
- Subagents, if explicitly authorized, must use GPT-6 or newer models.
