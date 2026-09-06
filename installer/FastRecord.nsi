Unicode True
ManifestSupportedOS win10
RequestExecutionLevel user

!include "MUI2.nsh"

!ifndef APP_VERSION
!define APP_VERSION "0.1.0"
!endif

Name "Fast Record"
OutFile "..\dist\FastRecord-${APP_VERSION}-x64-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\Fast Record"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fast Record" "InstallLocation"
BrandingText "Fast Record"
Icon "..\assets\fast-record-icon.ico"
UninstallIcon "..\assets\fast-record-icon.ico"

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "Fast Record"
VIAddVersionKey "CompanyName" "Fast Record"
VIAddVersionKey "FileDescription" "Gravador de tela para Windows"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"

!define MUI_ABORTWARNING
!define MUI_ICON "..\assets\fast-record-icon.ico"
!define MUI_UNICON "..\assets\fast-record-icon.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "PortugueseBR"

Section "Fast Record" SecMain
    SectionIn RO
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    File "..\build-ci\Release\fast-record.exe"

    CreateDirectory "$SMPROGRAMS\Fast Record"
    CreateShortCut "$SMPROGRAMS\Fast Record\Fast Record.lnk" "$INSTDIR\fast-record.exe" "" "$INSTDIR\fast-record.exe" 0
    CreateShortCut "$DESKTOP\Fast Record.lnk" "$INSTDIR\fast-record.exe" "" "$INSTDIR\fast-record.exe" 0

    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fast Record" "DisplayName" "Fast Record"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fast Record" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fast Record" "Publisher" "Fast Record"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fast Record" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fast Record" "UninstallString" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$DESKTOP\Fast Record.lnk"
    Delete "$SMPROGRAMS\Fast Record\Fast Record.lnk"
    RMDir "$SMPROGRAMS\Fast Record"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Fast Record"
    RMDir /r "$INSTDIR"
SectionEnd

