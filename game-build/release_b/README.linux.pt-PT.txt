# source-sha256: 41f44dbb265da119223fc604e1af6953fcc0c51ad237bdcb6945357147ca4bbc
Esta é uma tradução automática do texto em inglês; pode conter erros. O texto em inglês é a versão oficial.

Pokopia PC @VERSION@ para Linux, SteamOS e Steam Deck

1. No Steam, instale o Proton Experimental (Biblioteca, Ferramentas; num Steam Deck, mude primeiro para o Modo de Ambiente de Trabalho).
2. Execute o PokopiaLauncher.AppImage e escolha a pasta do seu dump (base/ e updates/<version>/, cada uma com exefs/ e romfs/) ou um ficheiro .nup. A versão 2.0.1a é obrigatória.
3. Prima Set up. Da primeira vez, descarrega as ferramentas de compilação (Python e Zig, cerca de 100 MB), compila o pkp.exe e a cache de shaders neste PC, através do Proton. Pode pausar e continuar mais tarde.
4. Prima Play. O jogo corre através do Proton do Steam.

Se o PokopiaLauncher.AppImage não arrancar (um sistema sem FUSE): ./PokopiaLauncher.AppImage --appimage-extract-and-run
A partir do Steam (Modo de Jogo no Deck): adicione o PokopiaLauncher.AppImage como um jogo que não é do Steam, com as opções de arranque --play.

Atualizar: extraia a nova versão por cima da sua instalação. Se a atualização mudar o código do jogo, o launcher avisa: prima Set up again e ele volta a compilar o pkp.exe. Os seus shaders e saves são mantidos.

Comunidade e ajuda: https://discord.recompia.com

Licenças: LICENSE.txt e licenses/.
