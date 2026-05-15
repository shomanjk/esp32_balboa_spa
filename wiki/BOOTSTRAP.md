# One-time GitHub wiki bootstrap

GitHub does not create the `.wiki.git` backend until the **first wiki page** exists. Until then, `git push` and the **Publish wiki** workflow fail with `Repository not found`.

## Steps (once per repository)

1. Open [Create wiki page — Home](https://github.com/shomanjk/esp32_balboa_spa/wiki/_new?wiki%5Btitle%5D=Home).
2. Enter any short placeholder body (for example `# Home`) and click **Save page**.
3. Sync the real content from this repo:

   ```bash
   gh workflow run publish-wiki.yml --ref ESP32
   ```

   Or locally:

   ```bash
   ./scripts/push-github-wiki.sh
   ```

After bootstrap, every push to `wiki/**` on branch `ESP32` (or `main`) runs [`.github/workflows/publish-wiki.yml`](../.github/workflows/publish-wiki.yml) automatically.
