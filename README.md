# Ubuntu Custom ISO Builder

This repository allows you to create your own customized Ubuntu-based operating system.

## Features
- Download the latest Ubuntu ISO (or use an existing one)
- Customize packages, wallpaper, hostname, etc.
- Build a new bootable Ubuntu-based ISO

## How to Use

### 1. Download Ubuntu ISO (optional)
```bash
bash scripts/download-ubuntu.sh
```

If you already have `ubuntu-24.04.1-desktop-amd64.iso` in the `iso/` folder, it will be used automatically.

### 2. Customize
Edit `scripts/customize.sh` to add packages, change settings, etc.

Then run:
```bash
bash scripts/customize.sh
```

### 3. Build Custom ISO
```bash
bash scripts/build-iso.sh
```

Your customized ISO will be saved in the `out/` folder.

## Folder Structure
```
.
├── README.md
├── scripts/
│   ├── download-ubuntu.sh
│   ├── customize.sh
│   └── build-iso.sh
├── iso/          # Place original Ubuntu ISO here
└── out/          # Final custom ISO will appear here
```

## Requirements
- Ubuntu 22.04+ or Debian-based system
- `sudo` access
- At least 8GB RAM and 20GB free space

---

Create your own Ubuntu-based OS easily!