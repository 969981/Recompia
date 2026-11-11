# source-sha256: 41f44dbb265da119223fc604e1af6953fcc0c51ad237bdcb6945357147ca4bbc
Esta é uma tradução automática do texto em inglês; ela pode conter erros. O texto em inglês é a versão oficial.

Pokopia PC @VERSION@ para Linux, SteamOS e Steam Deck

1. No Steam, instale o Proton Experimental (Biblioteca, Ferramentas; em um Steam Deck, mude antes para o Modo Desktop).
2. Execute o PokopiaLauncher.AppImage e escolha a pasta do seu dump (base/ e updates/<version>/, cada uma com exefs/ e romfs/) ou um arquivo .nup. A versão 2.0.1a é obrigatória.
3. Aperte Set up. Na primeira vez ele baixa as ferramentas de compilação (Python e Zig, cerca de 100 MB), compila o pkp.exe e o cache de shaders neste PC, pelo Proton. Você pode pausar e continuar depois.
4. Aperte Play. O jogo roda pelo Proton do Steam.

Se o PokopiaLauncher.AppImage não iniciar (um sistema sem FUSE): ./PokopiaLauncher.AppImage --appimage-extract-and-run
Pelo Steam (Modo Jogo no Deck): adicione o PokopiaLauncher.AppImage como um jogo que não é do Steam, com as opções de inicialização --play.

Atualizando: extraia a nova versão por cima da sua instalação. Se a atualização mudar o código do jogo, o launcher avisa: aperte Set up again e ele recompila o pkp.exe. Os seus shaders e saves são mantidos.

Comunidade e ajuda: https://discord.recompia.com

Licenças: LICENSE.txt e licenses/.
