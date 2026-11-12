# source-sha256: 41f44dbb265da119223fc604e1af6953fcc0c51ad237bdcb6945357147ca4bbc
这是英文文本的机器翻译，可能包含错误；英文文本是权威版本。

Pokopia PC @VERSION@，适用于 Linux、SteamOS 和 Steam Deck

1. 在 Steam 中安装 Proton Experimental（库、工具；在 Steam Deck 上请先切换到桌面模式）。
2. 运行 PokopiaLauncher.AppImage，并选择你的游戏转储文件夹（base/ 和 updates/<version>/，两者都包含 exefs/ 和 romfs/）或一个 .nup 文件。必须使用版本 2.0.1a。
3. 按 Set up。首次运行时，它会下载构建工具（Python 和 Zig，约 100 MB），并通过 Proton 在这台电脑上构建 pkp.exe 和着色器缓存。你可以暂停并稍后继续。
4. 按 Play。游戏通过 Steam 的 Proton 运行。

如果 PokopiaLauncher.AppImage 无法启动（系统没有 FUSE）：./PokopiaLauncher.AppImage --appimage-extract-and-run
从 Steam 启动（Deck 上的游戏模式）：将 PokopiaLauncher.AppImage 添加为非 Steam 游戏，并将启动选项设为 --play。

更新：将新版本解压到现有安装中覆盖。如果更新更改了游戏代码，启动器会提示你：再次按 Set up，它会重新构建 pkp.exe。你的着色器和存档会保留。

社区与帮助：https://discord.recompia.com

许可证：LICENSE.txt 和 licenses/。
