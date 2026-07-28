# Fork maintenance

This document explains how this repository relates to earlier projects and how we use **branches**, **pull requests**, and **tags**.

## Lineage

This codebase is a port and modernization of Balboa spa control firmware for ESP32. The ESP32-era README baseline descends from **[NorthernMan54/esp32_balboa_spa](https://github.com/NorthernMan54/esp32_balboa_spa)** (branch **`ESP32`**); a verbatim snapshot of that README is preserved at the bottom of [README.md](README.md) for credit and history. The [README](README.md) **Background and credits** section also cites:

- [cribskip/esp8266_spa](https://github.com/cribskip/esp8266_spa) (ESP8266 reference implementation)
- [EmmanuelLM/esp8266_spa](https://github.com/EmmanuelLM/esp8266_spa) (related ESP8266 work)
- [ccutrer/balboa_worldwide_app](https://github.com/ccutrer/balboa_worldwide_app) (protocol documentation)

The web UI credits [jozefnad/balboa-spa](https://github.com/jozefnad/balboa-spa). Those upstreams are **not** expected to receive pull requests from this fork; they appear **inactive or archival** for this use case. **Ongoing development happens in this repository.**

## Licensing posture for this fork

- **Firmware** (everything except `balboa-spa/` and third-party carve-outs):
  [PolyForm Noncommercial 1.0.0](LICENSE-firmware). Noncommercial use is
  permitted; **commercial use requires separate permission** from the licensor.
- **Web UI** (`balboa-spa/` submodule): Apache-2.0 (`balboa-spa/LICENSE`).
  Commercial use is allowed when Apache-2.0 conditions are met.
- **Overview and scope:** [`LICENSE`](LICENSE).
- **Third-party attributions:** [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
- Upstream license headers in vendored files must be preserved when distributing source.

## Optional: compare with an “upstream” remote

If you want to diff this fork against an original repo locally:

```bash
git remote add upstream https://github.com/cribskip/esp8266_spa.git   # example
git fetch upstream
```

Use GitHub’s **compare** view or `git diff` as needed. There is no requirement to stay merge-compatible with ESP8266-era layouts.

## Git workflow: push, PRs, and tags

Default branch for this fork is **`ESP32`**. Solo maintenance is fine; **prefer pull requests for substantive work** so [Codex](https://developers.openai.com/codex/integrations/github) and GitHub Actions can review the diff before merge. Direct pushes remain OK for tiny docs/typos or when you explicitly choose them.

**PR compile CI:** [`.github/workflows/build.yml`](.github/workflows/build.yml) runs `pio run` for **`M5AtomLite-tub`** and **`ESP32ota`** on pull requests and pushes to **`ESP32`**. The job copies **`src/config-example.h`** → **`src/config.h`** on the runner only (never commit private `config.h`). Wait for those checks plus Codex (or `@codex review`), triage findings, then merge. Wiki publish ([`publish-wiki.yml`](.github/workflows/publish-wiki.yml)) is separate and is not a PR gate.

| Action | When to use |
|--------|-------------|
| **Feature branch + PR** into **`ESP32`** | **Preferred** for firmware, protocol, MQTT, web UI, release-bound, or otherwise risky changes. Open the PR, wait for **Build** checks + Codex (or comment `@codex review`), triage findings, then merge. |
| **`git push`** straight to **`ESP32`** | OK for tiny docs/typos, throwaway experiments, or an explicit direct-push choice. Not the default for behavior changes. |
| **Pull request to another user’s repo** | Only if you decide to contribute upstream later. **Not** the default plan for this fork. |
| **Tags + GitHub Releases** | When you want a **named snapshot** others can pin (e.g. `v0.1.0`). Tag **after** the commits you want are on **`ESP32`**. |

### Suggested release flow

1. Land the work on **`ESP32`** via a merged feature-branch PR (preferred) or a deliberate direct push for trivial changes.
2. Update [CHANGELOG.md](CHANGELOG.md): move `[Unreleased]` items into a new `## [x.y.z] - YYYY-MM-DD` section. Use a **major** version (for example **`2.0.0`**) for milestone changes such as **reliable spa command writes**, per [Semantic Versioning](https://semver.org/) and the changelog narrative.
3. Align firmware **`VERSION`** in [`src/main.h`](src/main.h) and **`ANALYTICS_VERSION`** in [`lib/Analytics/Analytics.h`](lib/Analytics/Analytics.h) with the release tag (see [AGENTS.md](AGENTS.md)).
4. Create an annotated tag: `git tag -a v0.2.0 -m "v0.2.0"` (use the next version) then `git push origin v0.2.0`. The first release from this fork is **`v0.1.0`** (see [CHANGELOG.md](CHANGELOG.md)).
5. On GitHub: **Releases → Draft a new release**, choose the tag, paste changelog highlights.

**PRs** give you Build + Codex checkpoints; **tags** give others reproducible checkouts. You can still tag without a PR for a hotfix, but prefer merging via PR when the change is non-trivial.

## Issues and contributions

If **Issues** are enabled on this GitHub repository, they are the right place for bug reports and feature ideas **against this fork**. Pull requests **to this fork** are welcome when they match the project goals and licensing expectations of the original authors’ work.
