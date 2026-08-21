# ClassIn unofficial AppImage wrapper

## 📄 License & Disclaimer

This project is unofficially repackaged as an AppImage for demonstration and testing purposes to promote this packaging format to EEO. Consider this package as "experimental". I also invite you to request them to release an official AppImage, and if they agree, you can show this repository as a proof of concept.

This wrapper is licensed under the GPL-3.0-only license.

ClassIn is a registered trademark of Empower Education Online Ltd. (EEO). This project is community-maintained and is not affiliated with, supported, or endorsed by EEO. 

## Description

This is an unofficial AppImage wrapper for ClassIn. 

Since ClassIn only officially provides `.deb` packages (which cannot be installed directly on Red Hat, SUSE, Arch, Gentoo-based, or immutable Linux distributions), this wrapper packages and extracts the official `.deb` binaries when user builds the AppImage.

## ⚠️ Known Issues
  - **Screen sharing does not work on Wayland:** This is an upstream issue from ClassIn (EEO). The client lacks QtWayland and xdg-desktop-portal implementation, resulting in a black screen when attempting to share. A temporary workaround is running your session on X11, or use WMs or DEs that support it if your current DE removed X11 support (I will recommend WMs, for example, Openbox or i3).
  
If you encounter any other issues, please report them on [GitHub Issues](https://github.com/imngkhang/classin-appimage-wrapper/issues).

## 🚀 Quick Start

### Requirements

Before installing or building this package, ensure your system meets the following requirements:

- **Linux**: Any versions
- **Display Server**: X11 (recommended) or Wayland (with limitations)
- **Architecture**: `x86_64` or `aarch64`
- **Tools**: `jq`, `sha256sum`, `stat`, `wget`, `tar`, `ar`, `make`, from your distro

### Install the AppImage:
```bash
> **Note:** Releases will be available soon...
```

### Build from source

- **Debian / Ubuntu:**
  ```bash
  sudo apt update && sudo apt install build-essential jq wget tar coreutils
  ```

- **Fedora / Red Hat:**
  ```bash
  sudo dnf groupinstall "Development Tools" && sudo dnf install jq wget tar coreutils
  ```

- **Arch Linux / Manjaro:**
  ```bash
  sudo pacman -Syu --needed base-devel jq wget tar coreutils
  ```

- **openSUSE (Leap / Tumbleweed):**
  ```bash
  sudo zypper in -t pattern devel_basis && sudo zypper in jq wget tar coreutils
  ```

- **Gentoo:**
  ```bash
  sudo emerge --ask sys-devel/make sys-devel/binutils app-misc/jq net-misc/wget app-arch/tar sys-apps/coreutils
  ```

- **Locally build an AppImage:**
  ```bash  
  make
  ```

The `make` command will detect your architecture and build the corresponding AppImage. Type `make help` for more info. Final AppImage will be in `dist/` directory.

*Note: You can remove the development tools if you don't need, but DO NOT REMOVE other packages that installed.*

### Running ClassIn
```bash
chmod a+x ClassIn*.AppImage
./ClassIn*.AppImage
```

## 🤝 Contributing

Contributions are always welcome! You can help by:
- Reporting issues (crashes, problems, missing fonts/libs).
- Submitting PRs to update the runtime version or the wrapper.
- Improving metadata or manifest.

Want to become a **co-maintainer**? If you use ClassIn on Linux regularly, you can join in and help maintain this package!

Feel free to open an issue or submit a PR anytime!

