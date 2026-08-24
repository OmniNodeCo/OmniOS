# RAUC release signing

OmniOS system updates are RAUC `verity` bundles signed with X.509 keys. The image contains only a trust keyring; the private release key must never be committed or copied onto a target.

## Local development

Run `scripts/generate-rauc-keys.sh`. It creates an unencrypted, one-year self-signed development identity under `.keys/rauc/`, which Git ignores:

- `release.key.pem` — private bundle-signing key;
- `release.cert.pem` — signing certificate;
- `keyring.pem` — trust anchor embedded in the image.

For development, the signing certificate is also the trust anchor.

## Production

Use an offline CA and a protected release signer (encrypted secret, HSM, or isolated signing service). Supply `.keys/rauc/keyring.pem`, `.keys/rauc/release.cert.pem`, and `.keys/rauc/release.key.pem`, or override `OMNIOS_RAUC_KEYRING`, `RAUC_CERT_FILE`, and `RAUC_KEY_FILE` in a private Kas include. Never use the development generator for public release keys.

The CI template expects these base64-encoded secrets:

- `OMNIOS_RAUC_KEYRING_PEM_B64`
- `OMNIOS_RAUC_CERT_PEM_B64`
- `OMNIOS_RAUC_KEY_PEM_B64`

A key-rotation plan should include old and new trust anchors in a transition image. Increment `OMNIOS_RELEASE_REVISION` monotonically for each release; the resulting version is embedded as the next image's downgrade floor.

RPM repositories are supplemental. If published, use Yocto's `sign_rpm` class and signed HTTPS metadata; full OS releases use RAUC for atomic rollback.
