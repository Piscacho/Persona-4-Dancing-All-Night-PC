# Persona 4: Dancing All Night para PC

Port para PC de **Persona 4: Dancing All Night** (PlayStation 4, CUSA12112) para Windows 10/11.

> **No se incluyen archivos del juego.** Necesitas tu propia copia del juego de PS4 como carpeta
> (`eboot.bin`, `sce_module` y `sce_sys`).
> Persona y Persona 4: Dancing All Night son marcas de ATLUS / SEGA. Este proyecto no está
> afiliado a ellas.

## Descargar y jugar

1. Descarga el `.zip` desde la página de
   [Releases](https://github.com/Piscacho/Persona-4-Dancing-All-Night-PC/releases/latest).
2. Descomprímelo en una carpeta, por ejemplo `C:\Juegos\P4D`.
3. Pon la carpeta del juego junto a ella como `juego\CUSA12112` (se detecta sola), o elígela en
   el launcher: **Game → Carpeta del juego**.
4. Abre **`Persona 4 Dancing All Night.exe`** y pulsa **PLAY**. Si Windows muestra "Windows
   protegió tu PC", pulsa **Más información → Ejecutar de todas formas**.

- `Play Persona 4 Dancing All Night.exe` arranca el juego directamente con los ajustes guardados.
- Dentro del juego, **Insert** o **R3 + L2** abre el menú del port.

### Dónde se guarda todo

- Partidas, caché de shaders y registro: carpeta `user\` junto al launcher (consérvala al actualizar).
- Ajustes del juego: `p4d.ini`; ajustes del launcher: `%APPDATA%\p4d-launcher`.
- Registro de la última partida: `user\last_run.log`.

## Steam Deck y Linux (experimental)

> **Aún no está probado en una Steam Deck ni en un Linux real.** El launcher se comprueba en Debian,
> Fedora, Ubuntu y Arch, pero nadie ha jugado todavía con esta versión. Si la pruebas, funcione o no,
> cuéntanos cómo te fue con **Reportar un problema** (en el launcher) o abriendo un issue.

En la Steam Deck, haz esto en el **modo Escritorio**:

1. Descarga **`p4d-x86_64.AppImage`** desde
   [Releases](https://github.com/Piscacho/Persona-4-Dancing-All-Night-PC/releases/latest).
   Es un solo archivo: no hay que instalar nada.
2. Ábrelo con doble clic. Si no abre: clic derecho → **Propiedades → Permisos** → marca **Es ejecutable**.
3. En el launcher, elige la carpeta del juego (**Game → Carpeta del juego**, la que tiene `eboot.bin`) y,
   si tienes DLC ya extraídos (carpetas `ADDCONT…`), su carpeta en **Game → Carpeta de DLC**.
4. Cierra Steam (**Steam → Salir**) y en el launcher pulsa **Avanzado → Añadir a Steam**: crea el acceso con
   su portada y su logo.
5. Vuelve al **modo Juego**: aparece en tu biblioteca. Al abrirlo desde ahí entra directo al juego.

- **Actualizar:** el launcher avisa cuando hay versión nueva; pulsa **Actualizar** y reemplaza el
  AppImage él solo. Partidas y ajustes se conservan.
- Partidas, ajustes y registro se guardan en `~/.local/share/p4d` (el registro de la última partida es
  `user/last_run.log` ahí dentro).
- Los controles de la Steam Deck funcionan como un mando.

**Alternativa con Proton:** también puedes usar la versión de Windows. Descomprime el `.zip`, añade
**`Persona 4 Dancing All Night.exe`** a Steam con **Juegos → Añadir un juego que no es de Steam** (cambia el filtro a **Todos
los archivos**) y en **Propiedades → Compatibilidad** fuerza **Proton Experimental**.

## Requisitos

- Windows 10/11 de 64 bits.
- Tarjeta gráfica con Vulkan 1.3 y controlador actualizado.

## Créditos

- [shadPS4](https://github.com/shadps4-emu/shadPS4): núcleo de video de PS4 a Vulkan.

## Licencia

GNU GPL v2 o posterior ([LICENSE](LICENSE)). Los componentes de terceros mantienen sus propias licencias.
