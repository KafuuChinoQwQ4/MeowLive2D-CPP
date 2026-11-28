Unicode true
SetCompress off
!define PRODUCT "MeowLive2D"
!define BUNDLE_ID "io.meowlive.desktop"
!define MANUFACTURER "meowlive"
!define INSTALL_KEY "Software\${MANUFACTURER}\${PRODUCT}"
!define PREVIEW_KEY "Software\${BUNDLE_ID}"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
!define PREVIEW_UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${BUNDLE_ID}"
Name "${PRODUCT}"
OutFile "${ARTIFACT}"
InstallDir "placeholder\${PRODUCT}"
RequestExecutionLevel user
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "MeowLive2D"
VIAddVersionKey /LANG=1033 "FileDescription" "MeowLive2D Windows x64 Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
Var UpdateMode
Var Relaunch
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
  ${GetParameters} $1
  ClearErrors
  ${GetOptions} $1 "/UPDATE" $2
  IfErrors +2
    StrCpy $UpdateMode 1
  ClearErrors
  ${GetOptions} $1 "/P" $2
  IfErrors +2
    SetSilent silent
  IfSilent 0 locate
  ClearErrors
  ${GetOptions} $1 "/R" $2
  IfErrors +2
    StrCpy $Relaunch 1
locate:
  StrCmp $INSTDIR "placeholder\${PRODUCT}" 0 done
  ReadRegStr $0 HKCU "${INSTALL_KEY}" ""
  StrCmp $0 "" preview
  IfFileExists "$0\meowlive-desktop.exe" found
preview:
  ReadRegStr $0 HKCU "${PREVIEW_KEY}" "InstallLocation"
  StrCmp $0 "" fallback
  IfFileExists "$0\meowlive-desktop.exe" found
fallback:
  StrCpy $0 "$LOCALAPPDATA\Programs\${PRODUCT}"
found:
  StrCpy $INSTDIR $0
done:
FunctionEnd

Function WaitForInstalledProcess
  Exch $0
  Push $1
  Push $2
  StrCpy $1 80
retry:
  IfFileExists "$0" 0 ready
  System::Call 'kernel32::CreateFileW(w r0, i 0x40000000, i 1, p 0, i 3, i 0x80, p 0) p.r2'
  StrCmp $2 "-1" retry_wait
  System::Call 'kernel32::CloseHandle(p r2)'
  Goto ready
retry_wait:
  IntOp $1 $1 - 1
  StrCmp $1 "0" busy
  Sleep 250
  Goto retry
busy:
  Pop $2
  Pop $1
  Pop $0
  SetErrors
  Return
ready:
  Pop $2
  Pop $1
  Pop $0
  ClearErrors
FunctionEnd

Section "${PRODUCT}"
  SetRegView 64
  SetShellVarContext current
  ${If} $UpdateMode == 1
    ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "DisplayVersion"
    ${If} $0 != ""
      ${VersionCompare} "${VERSION}" "$0" $1
      ${If} $1 == 2
        SetErrorLevel 1
        Abort
      ${EndIf}
    ${EndIf}
    Push "$INSTDIR\meowlive-desktop.exe"
    Call WaitForInstalledProcess
    IfErrors process_busy
    Push "$INSTDIR\meowlive-server.exe"
    Call WaitForInstalledProcess
    IfErrors process_busy
  ${EndIf}
  !include "install-files.nsh"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${PRODUCT}.lnk" "$INSTDIR\meowlive-desktop.exe"
  WriteRegStr HKCU "${INSTALL_KEY}" "" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" '$\"$INSTDIR\meowlive-desktop.exe$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${MANUFACTURER}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "MainBinaryName" "meowlive-desktop.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" '$\"$INSTDIR$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  ReadRegStr $0 HKCU "${PREVIEW_KEY}" "InstallLocation"
  StrCmp $0 "$INSTDIR" 0 done
  DeleteRegKey HKCU "${PREVIEW_KEY}"
  DeleteRegKey HKCU "${PREVIEW_UNINSTALL_KEY}"
done:
  Goto installed
process_busy:
  DetailPrint "MeowLive2D is still running."
  SetErrorLevel 1
  Abort
installed:
SectionEnd

Function .onInstSuccess
  StrCmp $Relaunch "1" 0 done
  SetOutPath "$INSTDIR"
  Exec '$\"$INSTDIR\meowlive-desktop.exe$\"'
done:
FunctionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext current
  !include "uninstall-files.nsh"
  Delete "$INSTDIR\uninstall.exe"
  ReadRegStr $0 HKCU "${INSTALL_KEY}" ""
  StrCmp $0 "$INSTDIR" 0 done
  Delete "$SMPROGRAMS\${PRODUCT}.lnk"
  DeleteRegKey HKCU "${INSTALL_KEY}"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
done:
  RMDir "$INSTDIR"
SectionEnd
