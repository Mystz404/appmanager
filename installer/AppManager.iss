; ============================
; 预处理宏（可复用常量）
; ============================
; 应用显示名称（安装向导、开始菜单、卸载信息中会显示）
#define MyAppName "AppManager"
; 应用版本号（用于安装包版本与升级判断）
#define MyAppVersion "1.0.3.14"

; 发布者名称（控制面板卸载列表中的发布者）
#define MyAppPublisher "Kgooer"
; 主程序 EXE 文件名（安装后用于创建快捷方式与卸载图标）
#define MyAppExeName "AppManager.exe"
; 打包源目录（应为 windeployqt 后的完整可运行目录）
#define MySourceDir "D:\WorkSpace\QtProjects\AppManager\build\Desktop_Qt_5_15_2_MinGW_32_bit-Release\AppManager"
; 安装包输出目录（ISCC 编译后 exe 的保存位置）
#define MyOutputDir "D:\\WorkSpace\\QtProjects\\AppManager\\installer_output"

[Setup]
; 应用唯一 ID（GUID），用于标识同一个产品；已发布后不要随意改
AppId={{6E4F8F98-2DB5-4D92-9E3A-1D6F8F0B1C20}
; 应用名称（引用上面的宏）
AppName={#MyAppName}
; 应用版本（引用上面的宏）
AppVersion={#MyAppVersion}
; 安装包 EXE 的文件版本（Windows 版本资源）
VersionInfoVersion={#MyAppVersion}
; 安装包 EXE 的产品版本（Windows 版本资源）
VersionInfoProductVersion={#MyAppVersion}
; 发布者（引用上面的宏）
AppPublisher={#MyAppPublisher}
; 开始菜单程序组名称
DefaultGroupName={#MyAppName}
; 输出目录（生成安装包的位置）
OutputDir={#MyOutputDir}
; 输出安装包文件名（不含 .exe 后缀）
OutputBaseFilename=AppManagerSetup_{#MyAppVersion}
; 压缩算法（lzma 压缩率高）
Compression=lzma
; 固实压缩（体积更小，但安装时随机访问性能略差）
SolidCompression=yes
; 安装向导界面风格（modern 为现代样式）
WizardStyle=modern
; 在 64 位系统上默认安装到 64 位目录（x64compatible 适配 x64）
ArchitecturesInstallIn64BitMode=x64compatible
; 默认安装目录
DefaultDirName={userappdata}\AppManager
; 安装所需权限（admin 表示需要管理员权限）
PrivilegesRequired=admin
; 卸载条目显示图标（控制面板中展示的图标路径）
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
; 安装界面语言（简体中文）
Name: "chinesesimplified"; MessagesFile: "compiler:chinesesimplified.isl"

[Tasks]
; 可选任务：创建桌面快捷方式
; unchecked 表示默认不勾选
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"; Flags: checkablealone

[Files]
; 将源目录下所有文件递归复制到安装目录
; recursesubdirs: 递归子目录
; ignoreversion: 忽略文件版本比较，按规则直接复制
Source: "{#MySourceDir}\\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
; 开始菜单快捷方式
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
; 桌面快捷方式（仅当用户勾选 desktopicon 任务时创建）
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; 安装完成后运行主程序
; nowait: 不等待程序退出
; postinstall: 仅在安装向导最后一步显示
; skipifsilent: 静默安装时跳过自动启动 skipifsilent
; runasoriginaluser: 以当前登录用户身份启动，避免以管理员上下文运行导致行为异常
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; WorkingDir: "{app}"; Flags: nowait postinstall runasoriginaluser

[UninstallDelete]
; 递归删除安装目录下所有文件和子目录（包含运行时写入的配置、日志、应用文件等）
Type: filesandordirs; Name: "{app}"

[Code]
// 卸载完成后，若安装目录仍存在（可能残留空目录）则强制移除
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if DirExists(ExpandConstant('{app}')) then
      DelTree(ExpandConstant('{app}'), True, True, True);
    // 清除保存在注册表中的登录凭据
    RegDeleteValue(HKCU, 'Software\AppManager\AppManager', 'authToken');
    RegDeleteValue(HKCU, 'Software\AppManager\AppManager', 'authUser');
  end;
end;















