# 发版前回归套件（0.36.0 起，维护者的 Windows 测试机）。tools\regress\regress.ps1 -Suite 本文件 -Scope <改动范围> -Sdk <新 SDK> -Baseline <上一个发布版 SDK>
# 改了什么跑什么：-Scope 取下面 Scopes 里的名字，可以写几个，跑 Scopes 有交集的项；-Only 照旧可以点名。
# 占位符：{Sdk} {Baseline} {Label} {Out} {ItemOut} {Repo}，{Local:<键名>} 取本机配置的 Regress 一节。每项字段见 README.md。
# 用到的测试机（都是测试数据，不是生产；位置在本机配置的 Regress 一节）：
#   mas_6 / mas_7 / endfield：Regress.GameAvdHome（体检、明日方舟、1999、终末地）；gfx_t2：Regress.SrAvdHome（星铁）；
#   imesmoke：Regress.ImeAvdHome（输入法）；Regress.TestRoot 的 0 号（阴阳师，已装桥、已登录）和临时新建的 3、6、7 号。
@{
    Avds  = @('mas_0', 'mas_3', 'mas_4', 'mas_6', 'mas_7', 'gfx_t2', 'imesmoke', 'endfield')
    Scopes = @{
        emulator = '模拟器本体：qemu / gfxstream / crashpad 补丁、SDK 重编（跑全套）'
        bridge   = 'arm64 执行桥载荷（ModAVD bridge\ 的转发库、伴生库、.rc）'
        manager  = '管理器 / 命令行 modavd（ModAVDManager）'
        ime      = '宿主输入法 APK（ModAVDHostIME）'
        scripts  = '随包脚本（ModAVD package\：装桥脚本、补全组件、收集问题信息）'
        repack   = '只是重新打包（版本号的 P 加一，程序不变）'
    }
    # 打完包再用组包目录补跑的（不在本文件里，运行器最后按范围提示）
    PostPackage = @{
        'upgrade -Next <组包目录>' = @('emulator', 'bridge', 'manager', 'ime', 'scripts', 'repack')
        'ime-pkg -Package <组包目录>' = @('emulator', 'manager', 'ime', 'repack')
    }
    Items = @(
        @{
            Id = 'bench'; Title = '体检 `bench.ps1`'; Criterion = '全项通过，记冷启动秒数'
            Scopes = @('emulator')
            # 体检台和截图垫片是维护者仓库外的工具（Regress.Bench、Regress.ShimDll）
            Script = '{Local:Bench}'
            Args = @('-Sdk', '{Sdk}', '-Label', '{Label}', '-Avd', 'mas_6', '-AvdHome', '{Local:GameAvdHome}',
                '-ShimDll', '{Local:ShimDll}')
            TimeoutMin = 30
            Pass = @('\[PASS\] boot:', '\[PASS\] teardown:')
            # 宿主没开测试服务器时 net 报 ERROR、bench.ps1 退出码 1，和模拟器无关：退出码不算，只看这两组正则
            Fail = @('\[FAIL\]', '\[ERROR\] (?!net: no host test server)')
            ExitCode = 'any'
            Summary = '\[PASS\] boot: (cold boot [\d.]+ s)'
        }
        @{
            Id = 'ime'; Title = '宿主输入法通道 20 项'; Criterion = '全部通过'
            Scopes = @('emulator', 'ime')
            Script = '{Repo}\tools\regress\items\ime-reg.ps1'
            Args = @('-Sdk', '{Sdk}', '-Out', '{ItemOut}')
            TimeoutMin = 20
            Pass = @('汇总：PASS \d+，FAIL 0\b')
            Summary = '汇总：(PASS \d+，FAIL \d+)'
        }
        @{
            Id = 'bridge-default'; Title = '新建实例默认装执行桥，modavd bridge 卸 / 装'
            Criterion = '新建后有开机标记；第一次开机自己装好桥（shim-ctor OK、有撤回导出）；冷启动桥在、走 asg；modavd bridge uninstall 卸干净、install 又装上'
            Scopes = @('emulator', 'bridge', 'manager', 'scripts')
            Script = '{Repo}\tools\regress\items\bridge-default.ps1'
            Args = @('-Sdk', '{Sdk}')
            TimeoutMin = 30
        }
        # 游戏实跑：各自的自动化脚本真跑任务，只跑默认装桥、按包名分流的那一轮；
        # 帧率和上一个生产版的基准（tools\regress\baselines\fps.json）比，同机别的模拟器多于 2 个时只记录。
        # 明日方舟、1999 用的游戏号生产也在用（06:00 / 17:50）；那边的生产现在关着，所以没设 Avoid，生产恢复后加回
        # Avoid = @('04:30-07:00', '16:30-19:30')。
        @{
            Id = 'ark'; Title = '《明日方舟》MAA：StartUp + 集成战略 15 分钟（装桥）'
            Criterion = 'StartUp 完成、集成战略无任务链错误；游戏不重启；无 native crash / ANR / Vulkan 报错 / 新转储；有撤回行；帧率不低于上一版基准超过容差'
            Scopes = @('emulator', 'bridge')
            Script = '{Repo}\tools\regress\items\game-real.ps1'
            Args = @('-Game', 'ark', '-Sdk', '{Sdk}', '-Out', '{ItemOut}')
            TimeoutMin = 60
        }
        @{
            Id = 'r1999'; Title = '《重返未来：1999》M9A：启动游戏 + 智能均衡刷材料 15 分钟（装桥，耗体力一天一轮）'
            Criterion = 'M9A 启动游戏成功、刷材料成功或到时间停；游戏不重启；无 native crash / ANR / Vulkan 报错 / 新转储；帧率不低于上一版基准超过容差'
            Scopes = @('emulator')
            Script = '{Repo}\tools\regress\items\game-real.ps1'
            Args = @('-Game', 'r1999', '-Sdk', '{Sdk}', '-Out', '{ItemOut}')
            TimeoutMin = 50
        }
        @{
            Id = 'sr'; Title = '《崩坏：星穹铁道》SRC 模拟宇宙 15 分钟（装桥）'
            Criterion = 'SRC 跑满时间、无 Traceback；游戏不重启；无 native crash / ANR / Vulkan 报错 / 新转储；有撤回行；帧率不低于上一版基准超过容差'
            Scopes = @('emulator', 'bridge')
            Script = '{Repo}\tools\regress\items\game-real.ps1'
            Args = @('-Game', 'sr', '-Sdk', '{Sdk}', '-Out', '{ItemOut}')
            TimeoutMin = 60
        }
        @{
            Id = 'ef'; Title = '《明日方舟：终末地》MaaEnd：拜访好友、基建、日常奖励（装桥）'
            Criterion = 'MaaEnd 任务全部成功；游戏不重启；无 native crash / ANR / Vulkan 报错 / 新转储；有撤回行；帧率不低于上一版基准超过容差'
            Scopes = @('emulator', 'bridge')
            Script = '{Repo}\tools\regress\items\game-real.ps1'
            Args = @('-Game', 'ef', '-Sdk', '{Sdk}', '-Out', '{ItemOut}')
            TimeoutMin = 60
        }
        @{
            Id = 'bridge'; Title = 'arm64 执行桥装 / 卸全流程（PowerShell 5.1）'; Criterion = '不装桥零影响；照常开机时 install 退出码 3 只放标记；install -Reboot 核对全过；重复 install 不动；冷启动后桥和 zram 在；uninstall 卸干净'
            Scopes = @('emulator', 'bridge', 'scripts')
            Script = '{Repo}\tools\regress\items\bridge.ps1'
            Args = @('-Sdk', '{Sdk}')
            TimeoutMin = 30
        }
        @{
            Id = 'datadisk'; Title = '新建数据盘与扩容（modavd set --data-gb）'; Criterion = '新 16 GB 盘 inode 1048576；改大到 32 GB 后开机在线扩满；改小被拒'
            Scopes = @('emulator', 'manager')
            Script = '{Repo}\tools\regress\items\datadisk.ps1'
            Args = @('-Sdk', '{Sdk}')
            TimeoutMin = 20
        }
        @{
            Id = 'upgrade'; Title = '从上一个生产版本升级（照「怎么更新」整个覆盖新包）'; Criterion = '旧版建的实例照常开机、数据都在；版本号和包根新文件到位；旧盘改大后扩满；升级后 modavd 照常开机自动装好执行桥'
            # 只改输入法、只重新打包时打包前不跑这项：程序没变，打完包用组包目录跑的那次（PostPackage）才有意义
            Scopes = @('emulator', 'bridge', 'manager', 'scripts')
            Script = '{Repo}\tools\regress\items\upgrade.ps1'
            # 打包前 {Sdk} 是 SDK（包根文件取新编的 modavd 和仓库）；打完包再用 -Next <组包目录> 跑一次。
            # 起点只测生产现在跑的那一版，生产换版后跟着改（Regress.PrevPackage：那一版组包目录的固定副本，
            # 不放在会被重新组包覆盖的发版输出下面）
            Args = @('-Next', '{Sdk}', '-Prev', '{Local:PrevPackage}', '-Work', '{ItemOut}\work')
            TimeoutMin = 30
        }
        # 放在后面：测试根的 0 号和生产的 0 号同端口，生产在用时要等它关掉（最多 3 小时）
        @{
            Id = 'yys'; Title = '《阴阳师》冷启动进游戏、MaaYYs 自动御魂壹层 3 场（已装桥的实例）'
            Criterion = 'shim-ctor OK、有 zram、MaaYYs 认出庭院、无 Fatal signal；庭院 ≥ 45 fps；每场战斗 ≥ 55 fps 且无超过 1 秒的帧；读条最慢一帧 ≤ 2 秒（同机别的模拟器多于 2 个时帧率只记录，读条放宽到 3 秒）'
            Scopes = @('emulator', 'bridge', 'manager')
            Script = '{Repo}\tools\regress\items\yys.ps1'
            Args = @('-Sdk', '{Sdk}', '-Out', '{ItemOut}', '-MaaTask', '自动御魂',
                '-MaaCase', '大蛇挑战层数=壹层,是否切换御魂阵容=No,挑战方式=全自动,是否使用御魂加成=No,是否在战斗中投喂宠物=No',
                '-MaaInput', '自动挑战次数.挑战次数=3')
            TimeoutMin = 220
        }
        @{
            Id = 'ef-ab'; Title = '《明日方舟：终末地》冷启动，电脑空闲时与上一版交替各两轮'; Criterion = '到标题秒数无差别（宿主忙时测的不计入）'
            Scopes = @('emulator')
            Script = '{Repo}\tools\regress\items\ef-ab.ps1'
            Args = @('-Sdk', '{Sdk}', '-Baseline', '{Baseline}', '-Out', '{ItemOut}')
            NeedsBaseline = $true
            # 自己等宿主空下来（可用内存 ≥ 22 GB），最多 4 小时
            TimeoutMin = 330
        }
    )
}
