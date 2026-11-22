; TuneX - Copyright (C) 2026 Keinoaza
; Part of TuneX, a modified version of PitchNet. Licensed under the GNU
; Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
;
; Windows installer. It is built from an unpacked Windows release folder (the
; one that goes into TuneX-<version>-Windows-x64.zip), so it holds exactly
; the files of the zip. .github/workflows/windows-installer.yml runs:
;
;   ISCC /DAppVersion=<version> /DStageDir=<release folder> /DRedist=<vc_redist.x64.exe> packaging\windows\TuneX.iss
;
; It installs to the same places as the manual steps (INSTALL-Windows.txt),
; except the models, which go to the machine-wide folder the plug-in also
; reads (%PROGRAMDATA%\Keinoaza\TuneX\models), and it installs the
; Visual C++ Redistributable that the bundled onnxruntime.dll needs.
; The plug-in reads %APPDATA%\Keinoaza\TuneX\models instead whenever that
; folder exists (a manual install), so the README tells installer users to
; delete it.

#ifndef AppVersion
  #error Pass /DAppVersion=<version>
#endif
#ifndef StageDir
  #error Pass /DStageDir=<unpacked release folder>
#endif
#ifndef Redist
  #error Pass /DRedist=<path to vc_redist.x64.exe>
#endif

[Setup]
; Never change AppId: it is how a new version finds and replaces the old one.
AppId={{38373E75-C136-4F15-8F5D-6F966750C257}
AppName=TuneX
AppVersion={#AppVersion}
AppVerName=TuneX {#AppVersion}
AppPublisher=Keinoaza
AppPublisherURL=https://github.com/keinoaza666-stack/tunex
AppSupportURL=https://github.com/keinoaza666-stack/tunex/issues
; The licence texts and the uninstaller live here; the plug-in itself goes to
; the VST3 folder.
DefaultDirName={commonpf64}\Keinoaza\TuneX
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename=TuneX-{#AppVersion}-Windows-x64-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; English unless Windows is in Japanese.
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
UninstallDisplayName=TuneX
UninstallDisplayIcon={uninstallexe}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Messages]
english.FinishedLabelNoIcons=TuneX is installed. Restart your DAW and rescan plug-ins.
japanese.FinishedLabelNoIcons=TuneX をインストールしました。DAW を起動し直し、プラグインを再スキャンしてください。

[CustomMessages]
english.InstallingRedist=Installing the Microsoft Visual C++ Redistributable...
japanese.InstallingRedist=Microsoft Visual C++ 再頒布可能パッケージをインストールしています...

[InstallDelete]
; Start from an empty bundle so files an older version (or a manual install)
; left behind cannot mix with the new ones. The models folder is not emptied:
; users may add rmvpe.onnx there (see README), and the files below replace
; ours by name.
Type: filesandordirs; Name: "{commoncf64}\VST3\TuneX.vst3"

[Files]
Source: "{#StageDir}\TuneX.vst3\*"; DestDir: "{commoncf64}\VST3\TuneX.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\models\*"; DestDir: "{commonappdata}\Keinoaza\TuneX\models"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\MODIFICATIONS.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Redist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall

[Run]
; The redistributable does nothing when the same or a newer version is
; already installed. Its exit code is not checked: without it the plug-in
; still loads and tells the user how to fix it.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "{cm:InstallingRedist}"; Flags: waituntilterminated
