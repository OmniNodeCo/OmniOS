# RAUC release signing

OmniOS system updates are RAUC `verity` bundles signed with X.509 keys. The image contains only a trust anchor; the private signing key must never be committed or copied onto a target.

## Local development

Run `scripts/generate-rauc-keys.sh`. This creates an unencrypted, one-year development keypair under `.keys/rauc/`, which Git ignores. Both the image and its test update bundle are built against that identity.

## Production

Use an offline CA and a protected release signer (encrypted secret, HSM, or isolated signing service). Place build-time paths at `.keys/rauc/ca.cert.pem` and `.keys/rauc/ca.key.pem`, or override `OMNIOS_RAUC_KEYRING`, `RAUC_CERT_FILE`, and `RAUC_KEY_FILE` in a private Kas include. Never use the development generator for public release keys.

A key-rotation plan should include old and new trust anchors in a transition image. Increment `OMNIOS_RELEASE_REVISION` monotonically for each release; the resulting version is embedded as the next image's downgrade floor.

RPM repositories are supplemental. If published, use Yocto's `sign_rpm` class and signed HTTPS metadata; full OS releases use RAUC for atomic rollback.
