# 本机配置示例（tools\local.ps1 读它）。复制成同目录的 local.psd1 再填（local.psd1 不进 git），
# 或者放在仓库外、用环境变量 MODAVD_TOOLS_CONFIG 指过去。
# 留空（''、@()）就是没设：用到它的命令要你在命令行上给对应参数，否则报错。路径一律写绝对路径。
@{
    # ---- 共用
    # Python 3.12（只用标准库）。环境变量 MODAVD_PYTHON 优先；这里也空着就用 PATH 里的 python.exe
    Python         = ''
    # AOSP 模拟器源码树（repo 的根，下面有 external\qemu、hardware\google\gfxstream……）：编译、导出补丁、归档源码用
    EmuSrc         = ''
    # 临时输出的根目录：发版输出在 <TempRoot>\pubrel，编译日志在 <TempRoot>\build-sdk
    TempRoot       = ''
    # 输出目录和发版克隆绝不能落进去的目录（源码、备份、生产环境的根目录）。EmuSrc 和被打包的 SDK 总是算在内
    ProtectedRoots = @()
    # 写之前要求磁盘至少空出多少 GiB；不写就是 120
    # MinFreeGB    = 120

    # ---- 发版（tools\pubrel\release.ps1）
    # 私有隐私规则 privacy-private-rules.json（真名、私人邮箱、主机名、本机目录这类规则）：只放在本机、仓库外面，
    # 绝不提交。也可以用环境变量 MODAVD_PRIVATE_RULES。发版的隐私扫描没加载到它就停
    PrivateRules   = ''
    # 发版隐私扫描的缓存（文件哈希 → 上次扫过干净）：只在本机，不能在仓库和发版输出里。空着就是 <TempRoot>\pubrel\privacy-cache.json；
    # 扫描器或规则一变整个作废，release.ps1 -NoPrivacyCache 全部重扫；-DraftRelease（真要上传）那一轮不读缓存、全部重扫
    PrivacyCache   = ''
    # ModAVDManager、ModAVDHostIME、ModAVDExtras 的工作克隆（只读 tag，先 git fetch upstream --tags）
    ManagerRepo    = ''
    ImeRepo        = ''
    ExtrasRepo     = ''
    # 源码基准目录：没变的源码归档逐个核对冻结的哈希后复用（做法见 tools\pubrel\README.md「外部输入」）
    Baseline       = ''
    # 已审核的说明和许可证目录（说明.txt、LICENSES\）
    LicensePackage = ''
    # 随包的桌面 APK，放成 components\<文件名>；哈希固定在 tools\pubrel\config\components.SHA256SUMS.txt
    LauncherApk    = ''
    # 7-Zip 的 7z.exe；空着就找 PATH，再找 Program Files 下的
    SevenZip       = ''

    # ---- arm64 执行桥载荷（tools\gate\stage-payload.ps1）
    # 已冻结的载荷目录，里面就是 libmodavd_bridge.so、libmodavd_gate.so、modavd_bridge.rc；给了就只认它
    GatePayload    = ''
    # 不给 GatePayload 时两个 .so 各自的位置；空着就是 bridge\scripts 编出来的默认位置（%TEMP%\modavd-nbbridge\……）
    BridgeSo       = ''
    GateSo         = ''
}
