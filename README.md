# Ubuntu Custom ISO Builder

This repository helps you create your own customized Ubuntu-based operating system.

## Recommended Method: Cubic (Easiest & Most Reliable)

We **strongly recommend** using **[Cubic](https://github.com/PJ-Singh-UK/Cubic)** — a dedicated GUI tool for customizing Ubuntu ISOs.

### Why Cubic?
- Handles complex squashfs extraction properly
- Provides a real chroot environment
- Much more stable than manual scripts
- Supports Ubuntu 24.04+

### How to Use Cubic

1. **Install Cubic**:
   ```bash
   sudo apt-add-repository ppa:cubic-wizard/release
   sudo apt update
   sudo apt install cubic

    Run Cubic:

    Bash

    cubic

    Follow the GUI:
        Select your Ubuntu ISO
        Customize packages, files, settings, etc.
        Build your custom ISO

Alternative: Script-based Method (Advanced)

If you prefer scripts, you can still use them, but results may vary depending on the ISO.
Steps

Bash

# 1. Download Ubuntu ISO (or place it in iso/ folder)
bash scripts/download-ubuntu.sh

# 2. Customize (edit assets/packages.txt first)
bash scripts/customize.sh

# 3. Build the ISO
bash scripts/build-iso.sh

Your final ISO will appear in the out/ folder.