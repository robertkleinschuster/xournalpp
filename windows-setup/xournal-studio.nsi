; Side-by-side per-user installer for the Xournal Studio fork.

Unicode true
!include "MUI2.nsh"

Name "Xournal Studio ${XOURNALPP_VERSION}"
OutFile "${OUTPUT_INSTALLER_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Xournal Studio"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "InstallLocation"
RequestExecutionLevel user

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Xournal Studio" InstallXournalStudio
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    File /r "${SETUP_DIR}\*"

    CreateDirectory "$SMPROGRAMS\Xournal Studio"
    CreateShortcut "$SMPROGRAMS\Xournal Studio\Xournal Studio.lnk" "$INSTDIR\bin\xournalpp-wrapper.exe"
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "DisplayName" "Xournal Studio"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "DisplayVersion" "${XOURNALPP_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "Publisher" "Xournal Studio contributors"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio" "NoRepair" 1
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$SMPROGRAMS\Xournal Studio\Xournal Studio.lnk"
    RMDir "$SMPROGRAMS\Xournal Studio"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\XournalStudio"
    RMDir /r "$INSTDIR\bin"
    RMDir /r "$INSTDIR\lib"
    RMDir /r "$INSTDIR\share"
    RMDir /r "$INSTDIR\ui"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
