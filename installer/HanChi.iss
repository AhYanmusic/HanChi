; HanChi VST3 installer (Inno Setup 6)
; Compiled automatically by ISCC.exe on GitHub Actions

#define MyAppName "HanChi"
#define MyAppVersion "2.0.0"
#define MyAppPublisher "A-Yan Studio"

[Setup]
AppId={{8E1A2C4B-6D3F-4A5E-9B7C-1A2B3C4D5E6F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={code:GetDefaultDir}
DisableProgramGroupPage=yes
OutputDir=..
OutputBaseFilename=HanChi-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Files]
; The built HanChi.vst3 bundle must be copied to installer\payload\ by the workflow first
Source: "payload\HanChi.vst3\*"; DestDir: "{code:GetDefaultDir}\HanChi.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function GetDefaultDir(Param: string): string;
begin
  Result := 'C:\Program Files\Common Files\VST3';
end;
