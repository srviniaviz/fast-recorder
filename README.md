<p align="center">
  <img src="assets/fast-record-icon.png" width="96" alt="Ícone do Fast Record">
</p>

<h1 align="center">Fast Record</h1>

<p align="center">
  Um gravador de tela para Windows feito para sair do caminho.
</p>

O Fast Record vive na bandeja do sistema e abre um painel compacto quando você precisa dele. A ideia é simples: escolher a fonte, ajustar a qualidade e começar a captura sem atravessar assistentes ou telas desnecessárias.

> [!IMPORTANT]
> O projeto está em desenvolvimento. A captura do monitor em MP4 já funciona; áudio, captura de janela/região e o backend direto AMD AMF ainda estão em implementação.

## O que já está pronto

- painel flutuante com acesso pela bandeja do Windows;
- atalhos globais para gravar, pausar e parar;
- controles de gravar, pausar e parar;
- captura do monitor atual em MP4/H.264;
- opção de AV1 em MP4 quando o NVENC da NVIDIA anuncia suporte ao codec;
- codificação por software ou seleção automática do Media Foundation;
- codificação direta por NVENC quando a GPU NVIDIA compatível está disponível;
- resolução configurável de 720p a 4K;
- bitrate ajustável entre 4 e 80 Mbps;
- preferências restauradas depois de fechar e abrir o aplicativo;
- detecção em tempo de execução dos drivers NVENC e AMF;
- instância única: abrir o executável novamente traz o painel existente para frente.

Atalhos: **Ctrl + Shift + R** inicia ou encerra a gravação, **Ctrl + P** pausa ou continua e **Ctrl + Shift + S** encerra e salva o arquivo.

As preferências ficam em **HKEY_CURRENT_USER\Software\Fast Record**. Nenhuma configuração é enviada para fora do computador.

## Interface

A janela é nativa em C++, com a camada visual renderizada pelo WebView2. Isso mantém a integração com o Windows — bandeja, atalhos e captura — sem limitar a interface aos controles clássicos do Win32.

## Por baixo do painel

    TrayIcon + Hotkey
            │
            ▼
     AppController ───── AppSettings
            │
            ▼
    RecorderController
        ┌───┴──────────────┐
        ▼                  ▼
   NVENC direto       Media Foundation
   MP4 / H.264/AV1    MP4 / H.264

A captura usa Windows Graphics Capture, com redimensionamento pelo Direct3D 11. A proporção do monitor é preservada: resoluções diferentes recebem margens pretas quando necessário. O cursor é capturado pelo Windows.

No modo software/automático, os frames são copiados para memória antes da codificação H.264 pelo Media Foundation. No modo NVENC, a textura D3D11 da captura vai direto para o encoder da NVIDIA e o pacote H.264 ou AV1 é muxado em MP4 localmente. AV1 precisa ser usado com **NVENC** selecionado; o backend AMD AMF continua em espera para esta etapa.

Se o monitor for desconectado, sua resolução mudar ou a GPU falhar, o app tenta finalizar o MP4 e informa o erro. É necessário iniciar uma nova gravação após a mudança. Conteúdo protegido pode não aparecer; HDR ainda não tem tratamento de cor dedicado.

## Compilando

### Requisitos

- Windows 10 versão 1903 ou posterior, ou Windows 11;
- Visual Studio com o workload **Desktop development with C++**;
- Windows SDK;
- CMake 3.25 ou mais recente;
- Microsoft Edge WebView2 Runtime.

### Build básica

    cmake -S . -B build -A x64
    cmake --build build --config Release

O executável será criado em:

    build/Release/fast-record.exe

O SDK do WebView2 é obtido pelo CMake durante a configuração.

### Teste local da captura

O teste captura o monitor principal duas vezes, com pausa e retomada, em modo software e automático. Execute em uma sessão desktop interativa. Os vídeos permanecem na pasta indicada para inspeção:

    cmake --build build --config Debug --target capture-smoke
    build\Debug\capture-smoke.exe build\capture-smoke-output

Esse teste não roda no CI nem faz parte da build padrão.

## Releases

Todo push na branch `main` dispara o workflow de release. A versão é criada a
partir do número da execução, seguindo o formato `v0.1.<build>`; assim, cada
commit publicado recebe uma tag própria e não sobrescreve uma versão anterior.

Cada release publicado no GitHub contém:

- `FastRecord-0.1.<build>-x64-Setup.exe`, instalador por usuário;
- `FastRecord-0.1.<build>-x64.zip`, versão portátil.

O instalador cria atalhos no Menu Iniciar e na área de trabalho, registra o
desinstalador e não precisa de privilégios de administrador. O Microsoft Edge
WebView2 Runtime continua sendo um requisito do Windows.

### NVENC

O CMake busca automaticamente uma versão fixada dos headers do NVENC quando o SDK não está instalado. Para usar um SDK local:

    cmake -S . -B build -A x64 -DFASTRECORD_NVENC_SDK_DIR="C:\SDKs\Video_Codec_SDK"
    cmake --build build --config Release

O teste direto captura três segundos usando a GPU NVIDIA e gera um MP4 em `build/nvenc-smoke-output`:

    cmake --build build --config Debug --target nvenc-smoke
    build\Debug\nvenc-smoke.exe build\nvenc-smoke-output

Para exercitar o caminho AV1, passe `av1` como segundo argumento. O teste só
funciona em uma GPU NVIDIA cujo driver anuncia AV1:

    build\Debug\nvenc-smoke.exe build\nvenc-smoke-output-av1 av1

### Sonda completa do AMD AMF

    cmake -S . -B build -A x64 -DFASTRECORD_AMF_SDK_DIR="C:\SDKs\AMF"

Os SDKs são opcionais. Sem os headers, o Fast Record ainda verifica se as bibliotecas dos drivers estão instaladas e mantém a opção de software como fallback.

## Estrutura

    assets/          ícone e recursos visuais
    src/app/         ciclo de vida, janela e preferências
    src/platform/    integração com a bandeja do Windows
    src/recording/   estado do gravador e sondas NVENC/AMF
    src/resources/   recursos compilados no executável
    src/ui/          WebView2 e interface do painel

## Próximos passos

- [x] gravar o monitor atual em MP4/H.264;
- [x] migrar a captura para Windows Graphics Capture e Direct3D 11;
- [ ] capturar microfone e áudio do sistema com WASAPI;
- [x] conectar NVENC ao fluxo de frames;
- [x] adicionar escolha de codec H.264/AV1 ao NVENC;
- [ ] conectar AMD AMF ao fluxo de frames;
- [ ] implementar captura de janela e região;
- [ ] listar e reproduzir gravações recentes;
- [x] empacotar versões automaticamente pelo GitHub Actions.

## Licença

Ainda não há uma licença definida. Enquanto isso, o código permanece com todos os direitos reservados.
