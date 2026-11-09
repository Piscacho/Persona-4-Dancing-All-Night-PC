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

## Steam Deck y Linux (sin probar)

> **Aún no está probado.** No hay versión nativa para Linux: la idea es usar la de Windows con
> Proton, la capa de Steam que ejecuta juegos de Windows. Si lo pruebas, funcione o no, cuéntanos
> cómo te fue con el botón **Reportar un problema** del launcher (o abriendo un issue aquí).

En el Steam Deck, haz los pasos 1 a 4 en el **modo Escritorio**:

1. Descarga el `.zip` de [Releases](#descargar-y-jugar) y descomprímelo en una carpeta, por ejemplo
   `~/Juegos/P4D`.
2. Pon la carpeta del juego junto a ella como `juego/CUSA12112`, igual que en Windows. Los DLC ya extraídos
   (carpetas `ADDCONT…`) van en `dlc/` dentro de la carpeta del port.
3. En Steam: **Juegos → Añadir un juego que no es de Steam**. Pulsa **Examinar**, cambia el filtro de
   archivos a **Todos los archivos** y elige **`Persona 4 Dancing All Night.exe`**.
4. En la biblioteca, clic derecho sobre el juego → **Propiedades → Compatibilidad** → marca **Forzar el
   uso de una herramienta de compatibilidad de Steam Play** y elige **Proton Experimental** (o la
   versión más reciente).
5. Abre el juego desde Steam: se abre el launcher. Revisa que encuentre la carpeta del juego (si no,
   elígela en **Game → Carpeta del juego**; tu carpeta personal aparece como la unidad `Z:`) y pulsa
   **PLAY**.

- En el **modo Juego** del Steam Deck, el launcher se maneja con el trackpad o la pantalla táctil.
  Para entrar directo al juego, puedes añadir también **`Play Persona 4 Dancing All Night.exe`** como otro juego, con
  Proton igual que arriba: usa los ajustes que guardaste en el launcher.
- Los controles del Steam Deck funcionan como un mando de Xbox.
- Las partidas quedan en `user/` dentro de la carpeta del port. Los ajustes del launcher quedan dentro
  del prefijo de Proton de ese acceso directo de Steam.
- Necesitas un controlador con Vulkan 1.3 (el del Steam Deck y Mesa reciente lo tienen).

## Requisitos

- Windows 10/11 de 64 bits.
- Tarjeta gráfica con Vulkan 1.3 y controlador actualizado.

## Créditos

- [shadPS4](https://github.com/shadps4-emu/shadPS4): núcleo de video de PS4 a Vulkan.

## Licencia

GNU GPL v2 o posterior ([LICENSE](LICENSE)). Los componentes de terceros mantienen sus propias licencias.
