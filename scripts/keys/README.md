# TenBox archive signing key

`tenbox-archive-keyring.gpg` is the public half of the GPG key used to
sign the TenBox apt repository's `Release` / `InRelease` files at
`https://my.tenbox.ai/repo/dists/stable/`.

## How clients use it

`scripts/install-linux.sh` downloads a keyring from
`https://my.tenbox.ai/repo/tenbox-archive-keyring.gpg` and verifies
its sha256 against the value hard-coded in the script. The copy
committed here is the source of truth that the hard-coded sha256 must
match — bumping the key without bumping the script will break every
fresh install.

## Publishing a new key

When the key is rotated, drop the new public key here as
`tenbox-archive-keyring.gpg` (binary, dearmored — i.e. the output of
`gpg --export <KEYID>`, NOT `gpg --export --armor`). Update the
`expected_sha256` constant in [`scripts/install-linux.sh`](../install-linux.sh).

The full key-management runbook (where the private key lives, how to
generate / back up / revoke it) is internal — operators with cloud
access should consult the internal signing runbook.
