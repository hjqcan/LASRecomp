# Fork maintenance

- `main` and `beta_5` mirror the corresponding `peiikai/LASRecomp` branches. Update them only by fast-forwarding to upstream. Do not put fork features on either branch.
- `enhanced` is the maintained fork edition. Merge upstream `beta_5` updates into it; resolve conflicts by preserving the enhancements listed in `docs/enhanced.md` alongside upstream fixes.
- Do not reset `enhanced` to upstream, discard features rejected by upstream, or overwrite feature branches backing open PRs. Upstream submissions remain small, independent PRs against `beta_5` and follow the maintainer's scope requirements.
- Keep archive import (NSP/NSZ/XCI/XCZ), translated UI, version slots and experimental updated-version randomizer support in `enhanced`. They must stay out of upstream PRs unless upstream explicitly changes its policy.
- Multi-game work also belongs on `enhanced`; follow `docs/multi-game.md`. Keep common runtime fixes reusable, gate title-specific behavior by adapter, and preserve existing LAS behavior and saves. A catalog entry or successful compilation alone must not mark a new game playable.
- Verify the Windows package and the regression checklist in `docs/enhanced.md` before publishing a fork prerelease. Do not package game assets, generated game code, keys, saves, seeds or personal settings.
- Subagents, if explicitly authorized, must use GPT-6 or newer models.
