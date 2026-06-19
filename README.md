> [!IMPORTANT]
> **This is the `wepapered` patched fork of [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine).**
> It is used as a submodule by [wepapered](https://github.com/arca-inc/wepapered) and contains patches
> that are specific to that project (Hyprland daemon integration, shader cache, passthrough FBO fixes,
> CEF web wallpaper improvements, and more). It is **not** intended as a general-purpose drop-in
> replacement for the upstream project — please use the upstream if you don't need wepapered.
> See [CONTRIBUTORS.md](CONTRIBUTORS.md) for attribution details.

---

<p align="center">
	<a href="https://github.com/arca-inc/lwe-patched/blob/wepapered/LICENSE"><img src="https://img.shields.io/github/license/arca-inc/lwe-patched" /></a>
	<a href="https://github.com/arca-inc/lwe-patched/graphs/commit-activity"><img src="https://img.shields.io/github/commit-activity/m/arca-inc/lwe-patched?branch=wepapered" /></a>
	<a href="https://github.com/arca-inc/lwe-patched/graphs/contributors"><img src="https://img.shields.io/github/contributors/arca-inc/lwe-patched" /></a>
	<a href="https://github.com/arca-inc/lwe-patched/issues"><img src="https://img.shields.io/github/issues-raw/arca-inc/lwe-patched" /></a>
	<a href="https://www.codefactor.io/repository/github/arca-inc/lwe-patched"><img src="https://img.shields.io/codefactor/grade/github/arca-inc/lwe-patched" /></a>
</p>

# 🖼️ Linux Wallpaper Engine

Bring **Wallpaper Engine**-style live wallpapers to Linux! This project allows you to run animated wallpapers from Steam’s Wallpaper Engine right on your desktop.

> ⚠️ This is an educational project that evolved into a functional OpenGL-based wallpaper engine for Linux. Expect some limitations and quirks!

---

## 📦 Installation

> [!IMPORTANT]
> **This fork is not distributed as a standalone binary.**
> Installation is handled exclusively by [wepapered](https://github.com/arca-inc/wepapered), which
> builds this submodule as part of its own build process and manages the binary lifecycle via its
> daemon.
>
> **Do not clone or build this repository directly.** Please follow the
> [wepapered installation guide](https://github.com/arca-inc/wepapered#installation) instead.

---


## 🧪 Usage

> [!IMPORTANT]
> **Usage is managed exclusively by [wepapered](https://github.com/arca-inc/wepapered).**
> This fork is not meant to be invoked directly. The wepapered daemon handles wallpaper lifecycle,
> screen assignment, properties, and all runtime configuration through its own interface.
>
> Please refer to the [wepapered documentation](https://github.com/arca-inc/wepapered#usage) for usage instructions.

---

## 🙏 Special Thanks

- [RePKG](https://github.com/notscuffed/repkg) – for texture flag insights
- [RenderDoc](https://github.com/baldurk/renderdoc) – the best OpenGL debugger out there!

---

## 🔀 Upstream

This fork tracks [Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine).
All patches are applied on top of upstream commits; the `wepapered` branch diverges intentionally
and is **not** meant to be merged back upstream.

See [CONTRIBUTORS.md](CONTRIBUTORS.md) for the full contributor list.
