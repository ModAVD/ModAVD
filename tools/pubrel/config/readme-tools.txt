====================
管理器和命令行
====================

- ModAVD.exe：实例管理器，双击打开一个窗口，新建、修改、开关实例，看画面、装 APK、看日志。
  和 AUTO-MAS 用的是同一套实例，AUTO-MAS 正在挂机时不要在这里关它在用的实例。
- tools\modavd.exe：同样功能的命令行（modavd help 看全部命令），
  也能当 MCP 服务器（modavd mcp）给 AI 助手用。
- tools\modavd-bridge.ps1：给只有 arm64 版本的游戏（例如阴阳师）装 arm64 执行桥，
  用法见脚本开头的说明。
- extras\modavd_extras.dll：截图库，自动化工具用它不经 adb 直接取实例的画面。
  不用手动打开，也别挪走（工具按这个位置找它）；按 LGPL-3.0-or-later 发布。
- mumu-shim\shell\sdk\external_renderer_ipc.dll：给只认 MuMu 截图接口的工具用的兼容外壳。
  单独装的 MAA 要连 ModAVD 的实例，在 MAA 里把 MuMu 路径填成本目录下的 mumu-shim 文件夹。
  别挪走，也别改文件名；按 LGPL-3.0-or-later 发布。

