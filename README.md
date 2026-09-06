<p align="center">
  <img src="assets/fast-record-icon.png" width="96" alt="Ícone do Fast Record">
</p>

<h1 align="center">Fast Record</h1>

<p align="center">
  Um gravador de tela para Windows feito para sair do caminho.
</p>

O Fast Record vive na bandeja do sistema e abre um painel compacto quando você precisa dele. A ideia é simples: escolher a fonte, ajustar a qualidade e começar a captura sem atravessar assistentes ou telas desnecessárias.

> [!IMPORTANT]
> O projeto está em desenvolvimento. A interface, as preferências e a detecção dos encoders já funcionam; o pipeline que captura e grava o vídeo ainda não está conectado.

## O que já está pronto

- painel flutuante com acesso pela bandeja do Windows;
- atalho global **Ctrl + Shift + R**;
- controles de gravar, pausar e parar;
- seleção de tela inteira, janela ou região;
- escolha entre NVENC, AMD AMF e codificação por software;
- resolução configurável de 720p a 4K;
- bitrate ajustável entre 4 e 80 Mbps;
- preferências restauradas depois de fechar e abrir o aplicativo;
- detecção em tempo de execução dos drivers NVENC e AMF;
- instância única: abrir o executável novamente traz o painel existente para frente.

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
            │
            ├── NVENC  · NVIDIA
            ├── AMF    · AMD
            └── Software / Media Foundation

O plano para a captura é usar Windows Graphics Capture e WASAPI, compartilhar as superfícies pelo Direct3D 11 e entregar os frames diretamente ao encoder selecionado.

## Compilando

### Requisitos

- Windows 10 ou 11;
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

### Sonda completa do NVENC

    cmake -S . -B build -A x64 -DFASTRECORD_NVENC_SDK_DIR="C:\SDKs\Video_Codec_SDK"

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

- [ ] capturar o desktop com Windows Graphics Capture;
- [ ] capturar microfone e áudio do sistema com WASAPI;
- [ ] conectar NVENC e AMD AMF ao fluxo de frames;
- [ ] multiplexar vídeo e áudio em MP4;
- [ ] implementar captura de janela e região;
- [ ] listar e reproduzir gravações recentes;
- [ ] empacotar uma versão instalável.

## Licença

Ainda não há uma licença definida. Enquanto isso, o código permanece com todos os direitos reservados.

