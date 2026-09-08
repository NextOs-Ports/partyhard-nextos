# Party Hard GO 1.0.8-rc1 — Teste externo / External test

## Português

Este é um candidato de testes, sem aprovação física desta versão. O código foi
verificado no computador; o teste das funções de entrada do Unity em QEMU não
substitui o aparelho, a GPU nem o botão R3 real.

Faça uma cópia da instalação e dos saves antes de aplicar o ZIP. Extraia-o no
diretório de Ports, substituindo o launcher `Party Hard GO.sh` e os arquivos
do pacote em `partyhard/`. Preserve `gamedata/`, `home/` e seus arquivos
editáveis. O APK compatível continua sendo fornecido pelo dono; veja
`INSTALLATION.md`. Abra pelo frontend para exercitar o ambiente do firmware.

1. **dArkOS:** confirmar que passa pela extração/validação, pela splash de cinco
   segundos e chega ao jogo. Quando o PortMaster fornecer um banco grande,
   o diagnóstico deve mostrar `CONTROLLER ENV: transport=portmaster-file`.
2. **ROCKNIX/Aurnix e dArkOS:** no menu, usar o analógico direito para posicionar
   a ponta da seta sobre uma opção. R3 deve clicar exatamente nela. Repetir no
   alto, centro e parte inferior da tela, incluindo o seletor de controles.
3. Repetir o clique com o modo de vídeo padrão e com `preserve`, caso usado.
   Voltar à configuração anterior depois do teste.
4. Confirmar que D-pad/A continuam navegando/confirmando nativamente, que o
   gameplay mantém os controles próprios e que soltar diagonais para o movimento.
5. Testar pausa, retorno ao menu, áudio, saves e saída com SELECT+START.

No retorno, informar modelo exato, versão do firmware e versão do candidato;
se a seta move; se R3 não produz ação ou clica em outro lugar; em qual tela
acontece. Compartilhar o bundle sanitizado do nxobs quando disponível. Não
reinstalar o APK só por causa do antigo erro `Argument list too long`.

Publicação definitiva somente após aprovação dos testes deste ZIP exato.

## English

This is an externally tested candidate; this version has no physical approval.
Host checks and QEMU execution of Unity input functions do not test the device,
GPU or real R3 button.

Back up the installation and saves. Extract the ZIP into Ports, replacing
`Party Hard GO.sh` and the packaged files inside `partyhard/`. Keep `gamedata/`,
`home/` and owner configuration files. Supply your compatible APK as described
in `INSTALLATION.md`. Launch through the firmware frontend.

1. On dArkOS, verify startup reaches the game after the data gate and five-second
   splash. A large PortMaster database uses the diagnostic
   `CONTROLLER ENV: transport=portmaster-file`.
2. On ROCKNIX/Aurnix and dArkOS, use the right stick and R3 to select options
   under the arrow tip at the top, center and bottom, including controls selection.
3. Repeat in the default video mode and `preserve` if used; restore your setting.
4. Check native D-pad/A navigation, gameplay controls and diagonal release.
5. Check pause, returning to menus, audio, saves and SELECT+START exit.

Report the exact device, firmware and candidate version, whether the arrow moves,
whether R3 does nothing or clicks elsewhere, and the affected screen. Share the
sanitized nxobs bundle when available. Stable publication requires approval of
these exact ZIP bytes.
