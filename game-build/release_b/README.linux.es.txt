# source-sha256: 41f44dbb265da119223fc604e1af6953fcc0c51ad237bdcb6945357147ca4bbc
Esta es una traducción automática del texto en inglés; puede contener errores. El texto en inglés es la versión definitiva.

Pokopia PC @VERSION@ para Linux, SteamOS y Steam Deck

1. En Steam, instala Proton Experimental (Biblioteca, Herramientas; en Steam Deck, cambia primero al modo de escritorio).
2. Ejecuta PokopiaLauncher.AppImage y elige la carpeta de tu volcado (base/ y updates/<version>/, cada una con exefs/ y romfs/) o un archivo .nup. Se necesita la versión 2.0.1a.
3. Pulsa Set up. La primera vez descarga las herramientas de compilación (Python y Zig, unos 100 MB) y compila pkp.exe y la caché de sombreadores en este PC mediante Proton. Puedes pausar y continuar más tarde.
4. Pulsa Play. El juego se ejecuta a través de Proton de Steam.

Si el archivo PokopiaLauncher.AppImage no se inicia en un sistema que no tiene FUSE: ./PokopiaLauncher.AppImage --appimage-extract-and-run
Para iniciarlo desde Steam (modo Juego en Deck): añade PokopiaLauncher.AppImage como un juego que no sea de Steam y escribe --play en las opciones de lanzamiento.

Para actualizar, extrae los archivos de la nueva versión sobre los de tu instalación. Si la actualización cambia el código del juego, el launcher te avisa: pulsa Set up otra vez para que vuelva a compilar pkp.exe. Tus sombreadores y partidas guardadas se conservan.

Comunidad y ayuda: https://discord.recompia.com

Licencias: consulta LICENSE.txt y la carpeta licenses/.
