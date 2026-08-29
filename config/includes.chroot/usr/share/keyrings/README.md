# OmniOS archive keyring

`omnios-archive-keyring.asc` belongs here. It is the **public** half of the key
that signs the OmniOS APT repository, and it is what an installed system uses to
verify updates before installing them. It is not secret.

To create it, run the **Create OmniOS signing key** workflow once, download the
`omnios-signing-key` artifact, and commit the `omnios-archive-keyring.asc` file
from it into this directory.

Until that file exists, `/etc/apt/sources.list.d/omnios.sources` refers to a
keyring that is not present, so `apt update` will report the OmniOS repository
as unverifiable and skip it. Debian updates are unaffected.
