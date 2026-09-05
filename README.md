# Party Hard GO — NextOS AArch64 port

**Language / Idioma:** [English](#english) · [Português](#português)

## English

Party Hard GO 0.100038 runs through a native AArch64 Unity 6 IL2CPP loader and
the system SDL2/EGL/GLES2 stack. The finished adapter preserves Unity's Android
startup order, delivers the opening cutscene, menus and full gameplay, outputs
FMOD audio, supports the physical controller and draws a pointer arrow for the
game's touch-only dialogs.

The public package is BYO-data: it contains the port and its open-source
extraction tools, never tinyBuild's APK, native Android modules, or game
assets.

### Architecture

- Loads the original AArch64 `libmain.so`, `libunity.so`, and `libil2cpp.so`
  with their init arrays and `JNI_OnLoad` calls in the game's native order.
- Recreates the Android/JNI, filesystem, threading, lifecycle, input, and
  audio contracts used by Unity `6000.3.10f1` without an Android emulator.
- Uses only the SDL2 supplied by the firmware. The package does not vendor,
  preload, or redirect to a private SDL copy.
- Presents through EGL/GLES2 (SDL-owned context on KMS/Wayland firmwares, the
  vendor EGL path on Mali-450 fbdev). The Unity 6 GLES3 requests are bridged
  to the real GLES2 context; the graphics contract requires GLES 2.0 with
  ESSL 1.00, samples the framebuffer immediately before present and promotes
  graphics evidence only after the first real present returns.
- Adds the V5 owner video policy without touching Unity's renderer: `auto`
  fills every panel by default, while `preserve` keeps the internal 16:9
  image centered (for example 720x405 on a 720x720 display).

### Data preparation

The Android build ships its scenes and resources as a Play Asset Delivery pack
(`datapack.unity3d`). On first extraction the port:

1. validates the compatible APK (package ID, ARM64 ABI, critical payloads);
2. extracts only the Unity data and the three original ARM64 libraries;
3. unbundles the asset pack into the seven loose serialized files the engine
   already probes (`resources.assets`, `.resS`, `sharedassets1/2`, `level1`,
   `level2`) — byte for byte, nothing is reserialized.

The transformation is deterministic, hash-gated, transactional, and runs only
from owner-provided data.

### Other solved boundaries

- The Play licensing gate is answered locally through the game's own
  `LicensingServiceCallback` proxy.
- `PlayerPrefs.apply()` is asynchronous as on Android: disk is written on
  pause, exit and at most every 15 s (a synchronous write per `set` cost the
  frame rate on SD cards).
- JNI local references die at the frame boundary; reclaimed objects become
  tombstones, so a long session keeps a flat heap.
- FMOD PCM is sent to the firmware through SDL audio.
- The low-glibc runtime requires at most GLIBC 2.27 (public ceiling 2.30).

### Controls

The editable `NEXTOSCONTROLLERS.gptk` (format `NEXTOS_CONTROLLERS/4`) sits in
front of the Android KeyEvent/MotionEvent stream the game's InControl already
consumes. The firmware SDL2 mapping stays the only physical authority. The V5
provider-aware C6 seam stages mappings only for the SDL provider actually in
use, preserving the native firmware mapping on every other provider.

| Control | Gameplay / Menu | Delivery |
|---|---|---|
| A | native action in gameplay; `partyhard.click` in menus | keycode 96 / Android touch |
| B / X / Y | `partyhard.action2..4` | Android keycodes 97 / 99 / 100 |
| L1 / R1 | `partyhard.bumper_left` / `partyhard.bumper_right` | 102 / 103 |
| START | `partyhard.pause` | 108; while the game is paused, Android BACK closes the pause |
| Left stick | `partyhard.move` (vector, radial deadzone 0.15) | AXIS_X / AXIS_Y |
| Right stick | menu pointer; native in gameplay | arrow / AXIS_Z + AXIS_RZ |
| R3 | menu click; native in gameplay | Android touch / keycode 107 |
| D-pad, L2 / R2, L3, SELECT | native | menu D-pad uses one KeyEvent edge; gameplay retains HAT |
| SELECT + START | sovereign exit chord (framework, outside the file) | |

Contexts are proven from the engine: the scenes of this build's
`BuildSettings` (`AndroidLicensePermissionResolver`, `MainMenu`, `cutscene`)
are `[menu]`; every scene loaded from the asset pack is `[gameplay]`;
`Time.timeScale == 0` inside a party is the pause and selects `[menu]`; an
empty (loading) scene keeps every control on the native passthrough. The
native passthrough is the approved Mali-450 behaviour, so a missing or invalid
owner file changes nothing. In a proven menu, the right stick moves the
pointer and A or R3 clicks; in gameplay all three return to the native game.
The arrow starts visible, returns when moved or clicked, and hides after four
idle seconds following its first click. The pointer and Android MotionEvent
share the exact final content rectangle, so the click stays under the arrow
with bars or stretching on any aspect ratio. Menu D-pad navigation is
edge-triggered once per press instead of also receiving a continuous HAT.

The port opens every admitted pad (`nxinput_padset`) and the exit chord counts
only when SELECT and START come from the same pad. No test harness lives in
this executable.

### Video settings

`NEXTOSSETTINGS.txt` uses `NEXTOS_SETTINGS/2`. Its default
`video.aspect=auto` fills the entire display. Owners of a 720x720 device can
set `video.aspect=preserve` to show the 16:9 image centered with opaque bars.
The accepted values are `auto`, `engine`, `preserve`, and `stretch`; an
invalid owner file is preserved and the package default is used safely.

### Game data and first launch

Place a legal compatible Party Hard GO 0.100038 APK in `partyhard/gamedata/`
and launch the port. NXExtract 1.3.0 validates and prepares it on the device;
the APK filename is irrelevant. See `INSTALLATION.md` for the exact tested
identity and full directory layout.

### Build and release composition

```bash
cd ports/partyhard
./build_universal.sh
```

The public runtime is AArch64 and is audited for GLIBC 2.27 or older. Release
1.0.4 uses the immutable V5 integration tag
`framework-v5-nxbootstrap-0.8.4-arkos-stale-sdl-20260905`, peeled commit
`657fb65a23b5c3b20040e76307b27e6470b1d17c`. It includes nxbootstrap 0.8.4,
nxcompat 0.5.3, nxinput 0.11.8, nxgenerator 0.4.5, and nxrelease 0.4.11. Every
component tree is pinned in `FRAMEWORK-PIN.json`; the port never follows a
moving branch or `latest`.

nxbootstrap 0.8.4 fixes the ArkOS/PortMaster system-SDL handoff when an
inherited `LD_PRELOAD` names a stale SDL through the dynamic-loader token
`$LIB`. If the adapter did not author a different preload, the bootstrap drops
only that inherited unresolved system-SDL entry. Valid mapped token paths are
preserved literally; adapter-authored, private, non-SDL, nested, ambiguous, or
otherwise unresolved overrides still fail closed. There is no `eval`, path
guessing, global `LD_PRELOAD` wipe, or private SDL in the package.

Every launch uses the generated framework launcher. After the data gate and
before the adapter/game, the canonical bilingual `NEXT OS` / `RETRO ELITE`
NXSplash remains visible for five seconds without a skip option.

### Physical validation

The unchanged AArch64 game runtime identified above was approved on two
independent device stacks: NextOS with Mali-450 at 1280x720 and dArkOS with
Mali-G31 at 640x480. Fullscreen video, audio, gameplay, one-step menu D-pad
navigation, the aligned A/R3 pointer click and SELECT+START exit all passed.
Release 1.0.4 changes only the generated launcher/framework pin and was gated
on the host; its exact ZIP has not been physically retested. The 1.0.3
candidate was invalidated after ArkOS proved its inherited SDL entry was stale.

### Source map and licenses

- `src/main.c`, `src/nx_elf.c`: native loader and exact Unity lifecycle.
- `src/jni.c`, `src/android.c`, `src/bionic.c`, `src/pthread_bridge.c`:
  Android and JNI compatibility.
- `src/egl.c`, `src/egl_sdl.c`, `src/gles3.c`, `src/unity6_shader.c`: EGL/GLES
  facade, GLES3→GLES2 bridge and presentation.
- `src/nxgl_frame_proof_adapter.c`, `src/st_graphics_contract.c` and the
  canonical `src/nxgl_graphics_*`: framebuffer proof and graphics contract.
- `src/audio.c`, `src/opensles_shim.c`: FMOD/PCM output.
- `src/input.c`, `src/input_gptk.c`, `vendor/nxinput/`: controller, pointer
  and the live GPTK runtime.
- `nxextract/`: public extraction, the asset-pack preparation hook and its
  vendored dependencies used on first launch.

Port code is GPL-3.0-only. NXSplash is MIT. Third-party extraction-tool
licenses are included beside their components. Party Hard GO and all original
game content remain copyright tinyBuild / Pinokl Games and their rights
holders.

## Português

Party Hard GO 0.100038 roda por um loader Unity 6 IL2CPP AArch64 nativo e pela
stack SDL2/EGL/GLES2 do sistema. O adapter finalizado preserva a ordem de boot
Android da Unity, entrega a cutscene de abertura, os menus e a gameplay
completa, toca o áudio FMOD, aceita o controle físico e desenha uma seta de
ponteiro para os diálogos touch-only do jogo.

O pacote público é BYO-data: contém o port e suas ferramentas livres de
extração, nunca o APK, módulos Android nativos ou assets da tinyBuild.

### Arquitetura

- Carrega `libmain.so`, `libunity.so` e `libil2cpp.so` AArch64 originais com
  init arrays e `JNI_OnLoad` na ordem nativa do jogo.
- Recria os contratos Android/JNI, arquivos, threads, lifecycle, input e áudio
  usados pela Unity `6000.3.10f1`, sem emulador Android.
- Usa somente a SDL2 entregue pelo firmware, sem SDL privada.
- Apresenta por EGL/GLES2 (contexto da SDL em KMS/Wayland, EGL do fabricante
  no Mali-450 fbdev). Os pedidos GLES3 da Unity 6 são traduzidos para o
  contexto GLES2 real; o contrato gráfico exige GLES 2.0 com ESSL 1.00,
  amostra o framebuffer imediatamente antes do present e só promove a
  evidência depois do primeiro present real.
- Acrescenta a política de vídeo V5 sem alterar o renderer da Unity: `auto`
  preenche qualquer painel por padrão e `preserve` mantém a imagem 16:9
  centralizada (720x405 numa tela 720x720).

### Preparação dos dados

O build Android entrega cenas e recursos num pacote Play Asset Delivery
(`datapack.unity3d`). Na primeira extração, o port valida o APK compatível,
extrai só os dados Unity e as três bibliotecas ARM64 originais e desmonta o
pacote nos sete arquivos serializados soltos que a engine já procura — byte a
byte, sem reserializar nada. A transformação é determinística, protegida por
hashes, transacional e roda somente sobre dados do dono.

### Outras fronteiras resolvidas

- O gate de licença da Play é respondido localmente pelo proxy
  `LicensingServiceCallback` do próprio jogo.
- `PlayerPrefs.apply()` é assíncrono como no Android: o disco é escrito na
  pausa, na saída e no máximo a cada 15 s.
- Referências locais de JNI morrem na fronteira do quadro; objetos recolhidos
  viram lápides, e uma sessão longa mantém o heap parado.
- O PCM do FMOD segue para o firmware pelo áudio SDL.
- O runtime exige no máximo GLIBC 2.27 (teto público 2.30).

### Controles

| Controle | Gameplay / Menu | Entrega |
|---|---|---|
| A | ação nativa no gameplay; `partyhard.click` nos menus | keycode 96 / toque Android |
| B / X / Y | `partyhard.action2..4` | keycodes Android 97 / 99 / 100 |
| L1 / R1 | `partyhard.bumper_left` / `partyhard.bumper_right` | 102 / 103 |
| START | `partyhard.pause` | 108; com o jogo pausado, o BACK do Android fecha o pause |
| Analógico esquerdo | `partyhard.move` (vetor, deadzone radial 0,15) | AXIS_X / AXIS_Y |
| Analógico direito | ponteiro nos menus; nativo no gameplay | seta / AXIS_Z + AXIS_RZ |
| R3 | clique nos menus; nativo no gameplay | toque Android / keycode 107 |
| Direcional, L2 / R2, L3, SELECT | native | D-pad usa uma borda KeyEvent no menu e mantém HAT no gameplay |
| SELECT + START | chord soberano de saída (framework, fora do arquivo) | |

Os contextos são provados pela engine: as cenas do `BuildSettings` deste build
(`AndroidLicensePermissionResolver`, `MainMenu`, `cutscene`) são `[menu]`;
toda cena carregada do pacote de assets é `[gameplay]`; `Time.timeScale == 0`
dentro de uma festa é o pause e seleciona `[menu]`; cena vazia (carregando)
mantém tudo no passthrough nativo — que é exatamente o comportamento aprovado
no Mali-450. O mapa editável agora usa `NEXTOS_CONTROLLERS/4`, e o seam C6 V5
decide pelo provider SDL realmente carregado. Em menu comprovado, o analógico
direito move a seta e A/R3 clicam; no gameplay os três voltam ao jogo
nativamente. A seta nasce visível, reaparece ao ser usada e some após quatro
segundos parada depois do primeiro clique. A seta e o MotionEvent Android usam
o mesmo retângulo final de conteúdo, mantendo o clique sob a ponta tanto com
barras quanto esticado, em qualquer proporção. O D-pad navega uma opção por
pressão no menu, sem a segunda rota HAT contínua.

### Opções de vídeo

`NEXTOSSETTINGS.txt` usa `NEXTOS_SETTINGS/2`. O padrão
`video.aspect=auto` preenche toda a tela. Em aparelhos 720x720, o dono pode
usar `video.aspect=preserve` para manter a imagem 16:9 centralizada com barras
opacas. Também são aceitos `engine` e `stretch`; arquivo inválido é preservado
e o port volta com segurança ao padrão do pacote.

### Dados do jogo e primeira abertura

Coloque um APK legal e compatível do Party Hard GO 0.100038 em
`partyhard/gamedata/` e abra o port. O NXExtract 1.3.0 valida e prepara os
dados no aparelho; o nome do arquivo é irrelevante. `INSTALLATION.md` traz a
identidade exata testada e o layout completo.

### Composição da versão 1.0.4

A versão 1.0.4 fixa a tag imutável
`framework-v5-nxbootstrap-0.8.4-arkos-stale-sdl-20260905`, commit
`657fb65a23b5c3b20040e76307b27e6470b1d17c`, com nxbootstrap 0.8.4 e
nxrelease 0.4.11. No provider SDL do sistema, a correção descarta somente a
entrada SDL herdada e obsoleta em `LD_PRELOAD` que usa `$LIB`, desde que o
adapter não tenha criado um preload diferente. Caminhos válidos já mapeados são
preservados literalmente; overrides do adapter, privados, não SDL, aninhados,
ambíguos ou com outra variável continuam falhando fechado. Não há `eval`,
adivinhação de caminho, limpeza global de `LD_PRELOAD` nem SDL privada.

### Validação física

O runtime AArch64 do jogo, mantido byte a byte, foi aprovado em duas stacks
independentes: NextOS com Mali-450 em 1280x720 e dArkOS com Mali-G31 em
640x480. Passaram vídeo em tela cheia, áudio, gameplay, D-pad de uma opção por
pressão nos menus, clique alinhado por A/R3 e saída por SELECT+START. A versão
1.0.4 altera somente o launcher gerado e o pin do framework; seu ZIP exato foi
validado no host e não recebeu novo teste físico. O candidato 1.0.3 foi
invalidado quando o ArkOS confirmou que a SDL herdada apontava para um arquivo
obsoleto.

O código do port é GPL-3.0-only. NXSplash é MIT. As licenças das ferramentas
de extração acompanham seus componentes. Party Hard GO e todo o conteúdo
original permanecem propriedade da tinyBuild / Pinokl Games.
