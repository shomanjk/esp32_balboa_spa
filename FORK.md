# Fork maintenance

This document explains how this repository relates to earlier projects and how we use **branches**, **pull requests**, and **tags**.

## Lineage

This codebase is a port and modernization of Balboa spa control firmware for ESP32. The ESP32-era README baseline descends from **[NorthernMan54/esp32_balboa_spa](https://github.com/NorthernMan54/esp32_balboa_spa)** (branch **`ESP32`**); a verbatim snapshot of that README is preserved at the bottom of [README.md](README.md) for credit and history. The [README](README.md) **Background and credits** section also cites:

- [cribskip/esp8266_spa](https://github.com/cribskip/esp8266_spa) (ESP8266 reference implementation)
- [EmmanuelLM/esp8266_spa](https://github.com/EmmanuelLM/esp8266_spa) (related ESP8266 work)
- [ccutrer/balboa_worldwide_app](https://github.com/ccutrer/balboa_worldwide_app) (protocol documentation)

The web UI credits [jozefnad/balboa-spa](https://github.com/jozefnad/balboa-spa). Those upstreams are **not** expected to receive pull requests from this fork; they appear **inactive or archival** for this use case. **Ongoing development happens in this repository.**

## Optional: compare with an “upstream” remote

If you want to diff this fork against an original repo locally:

```bash
git remote add upstream https://github.com/cribskip/esp8266_spa.git   # example
git fetch upstream
```

Use GitHub’s **compare** view or `git diff` as needed. There is no requirement to stay merge-compatible with ESP8266-era layouts.

## Git workflow: push, PRs, and tags

| Action | When to use |
|--------|-------------|
| **`git push`** to your default branch (e.g. `main`) | Normal way to publish work to **your fork** on GitHub. You do **not** need a pull request on GitHub just to update your own fork’s `main`. |
| **Pull request on your fork** (e.g. `feature/xyz` → `main`) | Optional. Useful for CI, review, or your own checklist before merging. Not required for solo development. |
| **Pull request to another user’s repo** | Only if you decide to contribute upstream later. **Not** the default plan for this fork. |
| **Tags + GitHub Releases** | When you want a **named snapshot** others can pin (e.g. `v0.1.0`). Tag **after** the commits you want are on the target branch. |

### Suggested release flow

1. Commit and push to `main` (or merge a feature branch via PR if you use that workflow).
2. Update [CHANGELOG.md](CHANGELOG.md): move `[Unreleased]` items into a new `## [x.y.z] - YYYY-MM-DD` section. Use a **major** version (for example **`2.0.0`**) for milestone changes such as **reliable spa command writes**, per [Semantic Versioning](https://semver.org/) and the changelog narrative.
3. Align firmware **`VERSION`** in [`src/main.h`](src/main.h) and **`ANALYTICS_VERSION`** in [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h) with the release tag (see [AGENTS.md](AGENTS.md)).
4. Create an annotated tag: `git tag -a v0.2.0 -m "v0.2.0"` (use the next version) then `git push origin v0.2.0`. The first release from this fork is **`v0.1.0`** (see [CHANGELOG.md](CHANGELOG.md)).
5. On GitHub: **Releases → Draft a new release**, choose the tag, paste changelog highlights.

**You can use tags without PRs**, and **PRs without tags**; for a maintained fork, **both** are useful: PRs for your own process, tags for everyone else’s reproducible checkouts.

## Issues and contributions

If **Issues** are enabled on this GitHub repository, they are the right place for bug reports and feature ideas **against this fork**. Pull requests **to this fork** are welcome when they match the project goals and licensing expectations of the original authors’ work.
