# One-time GitHub wiki bootstrap

**Repo-only:** This file lives under `wiki/` for discoverability but is **not** published to GitHub Wiki. CI skips it via `ignore: BOOTSTRAP.md` in [`.github/workflows/publish-wiki.yml`](../.github/workflows/publish-wiki.yml). [`scripts/push-github-wiki.sh`](../scripts/push-github-wiki.sh) skips it on local sync as well.

GitHub does not create the `.wiki.git` backend until the **first wiki page** exists. Until then, `git push` and the **Publish wiki** workflow fail with `Repository not found`.

## Steps (once per repository)

Try syncing first from the repo root (use the branch that contains your `wiki/**` changes — **`ESP32`** or **`main`**):

```bash
gh workflow run publish-wiki.yml --ref "$(git branch --show-current)"
```

If the wiki shows content from [`Home.md`](Home.md), you are done. If you still get **Repository not found**:

1. Open [Create wiki page — Home](https://github.com/shomanjk/esp32_balboa_spa/wiki/_new?wiki%5Btitle%5D=Home).
2. Enter minimal stub content (for example `# Home`) and click **Save page**. The next sync **replaces** this with [`wiki/Home.md`](Home.md).
3. Sync again:

   ```bash
   gh workflow run publish-wiki.yml --ref "$(git branch --show-current)"
   ```

   Or locally:

   ```bash
   ./scripts/push-github-wiki.sh
   ```

After bootstrap, every push to `wiki/**` on branch `ESP32` (or `main`) runs [`.github/workflows/publish-wiki.yml`](../.github/workflows/publish-wiki.yml) automatically.

## Source of truth and edits

**Edit `wiki/*.md` in the main repo** (PR or push to `ESP32` / `main`). CI mirrors that folder to the GitHub Wiki tab.

- **Do not rely on the GitHub Wiki web editor** for durable changes. The publish workflow uses `strategy: clone`: it clones `.wiki.git`, copies files from `wiki/`, commits, and pushes. **The next sync overwrites the live wiki with repo content** — it does not merge wiki UI edits into your tree. A contributor’s wiki-only edit disappears from the live page on the next publish (it may still appear in the wiki’s git history as an older revision).
- **Your normal process** (edit `wiki/` → merge → CI publish) **replaces** any unpublished wiki UI edits with whatever is in the repo at that commit.
- **To recover wiki UI edits into the repo**, copy them into `wiki/` manually, or add a separate `direction: pull` workflow ([github-wiki-action docs](https://github.com/Andrew-Chen-Wang/github-wiki-action#pulling-wiki-edits-back)) — not enabled by default here.

Local sync without CI: [`scripts/push-github-wiki.sh`](../scripts/push-github-wiki.sh) (also clones, copies from `wiki/`, commits, pushes — same override semantics).
