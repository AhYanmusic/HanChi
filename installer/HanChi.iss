; HanChi 汉齿 VST3 安装器（Inno Setup 6）
; 由 GitHub Actions 云端自动调用 ISCC.exe 编译

#define MyAppName "HanChi 汉齿"
#define MyAppVersion "1.1.0"
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
; 构建产物 HanChi.vst3（文件夹）需先由工作流复制到 installer\payload\ 下
Source: "payload\HanChi.vst3\*"; DestDir: "{code:GetDefaultDir}\HanChi.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function GetDefaultDir(Param: string): string;
begin
  Result := 'C:\Program Files\Common Files\VST3';
end;
