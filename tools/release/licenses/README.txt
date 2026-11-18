Licences and notices of Recompia
================================

Recompia is free software under the GNU General Public License, version 3 or later (LICENSE.txt in the folder above this one).
This folder holds the licence text of every other component a release package contains, one file per component, named
LICENSE.<component>.<licence>.txt, and the notices that go with them. Each text was taken from the upstream project at the version
the release is built from.

Notices
  THIRD_PARTY_NOTICES.md      what the runtime (pkp_rt) takes from which project, and under which licence
  THIRD_PARTY_RECOMP.md       the same for the lifter in tools/build/recomp
  THIRD_PARTY_CRATES.txt      the Rust crates the launcher is built from, with the licence text of each
  CREDITS.md                  the projects Recompia builds on
  BUNDLED_TOOLS.txt           the build tools that come in the package (Windows and Linux packages)

Components of the runtime (pkp_rt, libpkp_rt) and the game layer
  SDL3 (zlib), Dear ImGui (MIT), volk (MIT), VulkanMemoryAllocator (MIT), Vulkan-Headers (Apache-2.0 OR MIT), zstd (BSD-3-Clause),
  miniz (MIT), Modtopia (MIT), SMAA (MIT), NVIDIA Image Scaling (MIT), AMD FidelityFX FSR 1 and FidelityFX API headers (MIT),
  NVIDIA Streamline headers (MIT), the NVN header (MIT), libvpx and its x86inc macros (BSD-3-Clause, ISC; Windows builds),
  astc-encoder (Apache-2.0), bc7enc_rdo (MIT or public domain, bc7e under Apache-2.0), ISPC (BSD-3-Clause), DejaVu fonts
  (Bitstream Vera licence).
Code adapted from other projects
  pokopia-native and its co-op module pokopia-coop (MIT), SuperMarioWonderRecomp (MIT), Ryujinx / ryubing (MIT), totk-pc-recomp (MIT),
  libnx (ISC), LLVM libc++abi (Apache-2.0 with LLVM exception), yuzu and Eden (GPL-2.0-or-later), smo-recomp (GPL-3.0),
  Mesa NAK (MIT).
Shader tools: Sascha Willems' Vulkan samples (MIT, the stand-in shader corpus).
Launcher: Inter (OFL 1.1), Lucide (ISC), Font Awesome Free (CC BY 4.0, MIT, OFL 1.1), @tauri-apps/api (MIT or Apache-2.0), and the
  crates in THIRD_PARTY_CRATES.txt.
Save Editor: PokoHex (GPL-3.0) and BCnEncoder.Net (MIT or Unlicense; the data tool pokohex-extract).
Windows package: libusb (LGPL-2.1-or-later; the unchanged official libusb-1.0.dll). The packages' bundled build tools (Zig, Python,
  FSR 3 and Streamline libraries) have their licence texts in BUNDLED_TOOLS.txt's list, in this folder on Windows.

Source code of GPL and LGPL components
  Recompia: the release tag in https://github.com/Dittobyte/Recompia, which holds every file this package is built from, including the
    launcher, the runtime, the lifter and the scripts that make this package.
  PokoHex: the release page of this version carries PokoHex-source-<commit>.tar.gz. It is the PokoHex source at the commit the Save
    Editor is built from, with launcher.patch (the changes Recompia makes to it) and build-pokohex.sh, the script that applies them
    and builds the editor and pokohex-extract.
  libusb: the release page of this version carries libusb-1.0.30.tar.bz2, the source of the libusb 1.0.30 release this package's
    libusb-1.0.dll is built from. The DLL is loaded by name at run time, so any compatible build of libusb-1.0.dll can replace it.
