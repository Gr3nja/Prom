; ============================================================
;  Prom インストーラー (Inno Setup 6.3 以降)
;  先に build.bat で prom.exe をビルドしてから、このファイルをコンパイルします。
;  (make_installer.bat なら、ビルドからインストーラー作成まで一度に行います)
; ============================================================
#define AppName      "Prom"
#define AppVersion   "1.0.0"
#define AppExe       "prom.exe"
; WebView2 Runtime のインストーラーを同梱したい場合は、Microsoft の
; 「Evergreen Bootstrapper」(MicrosoftEdgeWebview2Setup.exe) をこのファイルと同じフォルダに置いてください。
#define WV2Boot      "MicrosoftEdgeWebview2Setup.exe"

[Setup]
AppId={{EE3CBE5A-A912-4DC7-8377-420A05016248}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppName}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile=prom.ico
OutputDir=installer
OutputBaseFilename=Prom-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "デスクトップにアイコンを作成する"; GroupDescription: "追加のアイコン:"; Flags: unchecked

[Files]
Source: "prom.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifexist WV2Boot
Source: "{#WV2Boot}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: not IsWebView2Installed
#endif

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
#ifexist WV2Boot
Filename: "{tmp}\{#WV2Boot}"; Parameters: "/silent /install"; StatusMsg: "WebView2 Runtime をインストールしています..."; Check: not IsWebView2Installed
#endif
Filename: "{app}\{#AppExe}"; Description: "{#AppName} を起動する"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; WebView2 のユーザーデータ (prom.exe が %LOCALAPPDATA%\PromWebView2 に作ります)
Type: filesandordirs; Name: "{localappdata}\PromWebView2"

[Code]
const
  WV2Key = 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';

function IsWebView2Installed: Boolean;
var
  v: String;
begin
  Result :=
    (RegQueryStringValue(HKLM32, WV2Key, 'pv', v) and (v <> '') and (v <> '0.0.0.0')) or
    (RegQueryStringValue(HKCU, WV2Key, 'pv', v) and (v <> '') and (v <> '0.0.0.0'));
end;

#ifnexist WV2Boot
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and (not IsWebView2Installed) then
    MsgBox('WebView2 Runtime が見つかりませんでした。' + #13#10 +
           'Prom の表示に必要です。次のページからインストールしてください:' + #13#10 +
           'https://developer.microsoft.com/microsoft-edge/webview2/', mbInformation, MB_OK);
end;
#endif
