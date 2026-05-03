# Release process

The Linux update path (cloud console "升级" button → daemon `apt-get
install --only-upgrade tenbox`) only works once a tag has been published
through this exact dance. Skipping a step leaves the apt repo without
the new deb and every paired host stuck on the old version.

## Steps

1. **Bump `VERSION`.** A single line, e.g. `0.7.6`. The GitHub Actions
   release workflow refuses to run if `v$(cat VERSION)` does not match
   the pushed tag.
2. **Update changelog.** Either edit the GitHub Release body once the
   workflow opens it (auto-generated from commits is the default), or
   fill in the `linux.latest.changelog_md` field in
   `tenbox-cloud/data/releases.json` after `sync_apt_repo.py` regenerates
   it. The console reads the latter for its in-app changelog modal.
3. **Commit the bump.**
   ```sh
   git commit -am "chore: release 0.7.6"
   git push
   ```
4. **Tag and push.**
   ```sh
   git tag v0.7.6
   git push origin v0.7.6
   ```
5. **Watch [`.github/workflows/release.yml`](../.github/workflows/release.yml).**
   It builds amd64 + arm64 debs, attaches them to the matching GitHub
   Release alongside `install-linux.sh`. Verify both `.deb` assets
   appear; the workflow runs on `ubuntu-22.04` / `ubuntu-22.04-arm`
   (NOT `ubuntu-latest` — see workflow header for why).
6. **Optional: kick the cloud sync.** The cloud's
   `tenbox-apt-sync.timer` polls every 10 minutes. To make a release
   visible to consoles immediately, ssh to the cloud host and run:
   ```sh
   sudo -u tenbox-cloud python3 \
       /data/tenbox-cloud/scripts/sync_apt_repo.py
   ```

## Rollback

Operators can stay on or revert to a known-good version with:

```sh
sudo apt-get install tenbox=0.7.5
```

Older debs remain in the apt repo's `pool/` until manually removed.

## Repository signing

The `dists/stable/Release` file at `https://my.tenbox.ai/repo/` is GPG
signed; clients verify it via `signed-by=` in their
`/etc/apt/sources.list.d/tenbox.list`. The public keyring lives at:

- Download: <https://my.tenbox.ai/repo/tenbox-archive-keyring.gpg>
- In-tree (source of truth): [`scripts/keys/tenbox-archive-keyring.gpg`](../scripts/keys/tenbox-archive-keyring.gpg)
- Sha256: hard-coded in [`scripts/install-linux.sh`](../scripts/install-linux.sh) (`expected_sha256`)

Verifying a downloaded keyring out of band:

```sh
gpg --show-keys --with-fingerprint \
    /etc/apt/keyrings/tenbox-archive-keyring.gpg
```

Key generation, storage, rotation, and revocation are operator
concerns and are documented in the internal signing runbook (not in
this repo).
