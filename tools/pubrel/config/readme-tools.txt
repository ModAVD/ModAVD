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

