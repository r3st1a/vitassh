# VitaSSH

A PlayStation Vita SSH client. Connects to a normal `sshd`, gives you a real
terminal with scrollback, and puts the keys a DualShock does not have
(Ctrl-C, Esc, F-keys) behind a panel you can reach with the d-pad.

Download the latest `vitassh-<x.y.z>.vpk` from
[GitHub Releases](https://github.com/r3st1a/vitassh/releases), or build it
yourself with the instructions below. The resulting package is installable from
VitaShell on a HENkaku/Enso console.

## 功能

- **终端模拟**：VT100/ANSI 子集 —— 光标定位、ED/EL、滚动区域、SGR 16 色、
  备用屏幕、512 行回滚、插入模式、制表位、DEC 字符集。`vim`、`top`、`htop`、
  `less` 都能跑。8x8 点阵字体按 1:1 像素绘制，120 列 × 51 行，清晰且不占地方。
- **主机管理**：应用内新建 / 编辑 / 删除主机，存回
  `ux0:data/vitassh/hosts.txt`，重启后仍在。端口用数字键盘，主机名和用户名关掉
  自动大写与候选词。
- **认证**：密码认证（`password`，服务器只提供 `keyboard-interactive` 时自动
  回退）。输错可重试 3 次而不必重连。
- **保存密码**（可选）：主机表单里的密码行可以把密码存下来，下次连接自动填，
  不再弹输入框。存的是密文——见「保存的密码」一节。
- **主机密钥**：首次连接显示 SHA-256 指纹，L 接受 / R 中止。
- **触摸键盘**：终端里点一下屏幕或按 Start 就在底部弹出自己的 QWERTY 键盘，
  按键直接走 SSH 通道，不经过系统输入法页面。Ctrl / Alt / Shift 是粘滞键，
  `Ctrl` 后按 `c` 就是 `Ctrl-C`；另有数字与符号页、方向键和长按连发。

## 构建

需要 VitaSDK（用 `VITASDK` 指向安装目录）、VitaSDK 的 `zlib` /
`openssl-1.1.1` / `libvita2d`，以及交叉编译好的 `libssh2`（1.11.1，OpenSSL
后端）。

```bash
export VITASDK='/path/to/vitasdk'   # Windows 上 toolchain 用反斜杠形式，如 'C:\vitasdk'
CM=cmake
NJ=ninja
# 没装 cmake/ninja 时：python -m pip install cmake ninja
# 若 pip 装了但不在 PATH：
# CM="$(python -c 'import cmake,os;print(os.path.join(cmake.CMAKE_BIN_DIR,"cmake.exe"))')"
# NJ="$(python -c 'import ninja,os;print(os.path.join(ninja.BIN_DIR,"ninja.exe"))')"

"$CM" -S . -B build/app -G Ninja -DCMAKE_MAKE_PROGRAM="$NJ" -DCMAKE_BUILD_TYPE=Release
"$CM" --build build/app
```

产物是 `build/app/vitassh.vpk`。发布时复制成 `dist/vitassh-1.0.0.vpk` 并作为
GitHub Release 附件上传（`dist/` 不进 git）。重跑 `node tools/make_assets.js`
才会重新生成 `sce_sys/` 下的图标和 LiveArea 图。

### 宿主机单元测试

终端核心（`term.c` / `vt.c` / `config.c`）不依赖任何 `psp2/` 头文件，可以在
Windows 上直接跑：

```bash
bash tools/hosttest.sh          # gcc
bash tools/hosttest.sh asan     # clang + ASan/UBSan
```

没有 gcc/clang 时可以用 Zig 当 C 编译器，源文件列表和参数见
`doc/HANDOFF.md` §3。

483 项检查，覆盖配置解析与组装、光标/擦除/插入、SGR、滚动区域、备用屏幕、
DSR/DA 应答、UTF-8，以及 `ls --color` / `top` / `vim` 的合成语料。其中配置部分
包含回归测试：满长度的 name/host/user 写盘后必须能原样读回——曾经的行缓冲按
`CFG_HOST_CAP * 2` 取值，装不下 `config_save` 能写出的最长行。密码库部分覆盖往返、
容量、超长字段、原子替换，以及一大组损坏样本：每个样本都从好文件出发翻转一个
字节或改一次长度，断言读回来要么可用且为空、要么不可用——绝不允许返回没写进去
的内容。宿主机没有 OpenSSL，所以跑的是 `test/vault_platform_stub.c`（一份只有
可测性的玩具实现，**不是**加密），真正的原语只在设备上自检。

## 安装与运行

1. 用 VitaShell 把 `vitassh.vpk` 拷到 `ux0:`，按 × 安装。
2. 首次启动会在 `ux0:data/vitassh/hosts.txt` 生成一份带注释的模板。
3. 在主界面按 **△** 新建一台主机，按 **□** 编辑选中的那台（表单里可以删除）。
   存盘后立即生效。

也可以直接改那个文件，一行一台主机：

```
name|host|port|user
home|192.168.1.10|22|root
box|10.0.0.5||admin
```

`port` 空则 22，`user` 空则 `root`，`name` 空则用主机名。`#` 开头是注释。
改完在主界面按 **SELECT** 重新读取。

## 按键

**主机列表**

| | |
|---|---|
| 十字键 | 选择主机 |
| × | 连接 |
| △ | 新建主机 |
| □ | 编辑选中的主机 |
| ○ | 退出 |
| SELECT | 重新读取 hosts.txt |

**主机编辑表单**（列表里按 △ 或 □ 打开）

| | |
|---|---|
| 十字键 | 切换字段 |
| × | 编辑该字段 / 保存 / 删除 |
| ○ | 取消；删除待确认时先取消确认 |

字段留空按规则取默认值：`name` 用主机名、`port` 用 22、`user` 用 `root`——
和手写文件完全一致（两边走的是同一个组装函数）。主机名不能留空，名字也不能以
`#` 开头（会被当成注释读掉）。端口只收数字，主机名和用户名不会被自动大写。
`Password` 行见「保存的密码」。删除要点两次 × 确认。

**终端（实时）**

| | |
|---|---|
| 十字键 | 方向键 |
| × | 回车 |
| □ | 退格；长按 = `Ctrl-U` |
| △ | Tab |
| Start / 点击屏幕 | 显示或收起触摸键盘 |
| SELECT | 控制键面板 |
| L | 进入回滚 |
| ○ | 断开，回主机列表 |

触摸键盘占屏幕下方 284 像素，弹出时终端整体上移，光标行始终留在键盘上方。
键盘分字母、数字、符号三页：底部左侧的 `123` / `ABC` 切换字母和数字，数字页的
`=<` 进入符号页。`Ctrl`、`Alt`、`Shift` 点一下是待发状态（再点取消），发完一个
键自动复位；`Shift` 连点两下锁定大写（标签变成 `Caps`）。`Del` 和方向键支持长按
连发。密码输入框仍使用系统输入法，因为密码可能包含非 ASCII 字符。

**终端（回滚模式，状态栏显示 `SCROLL +N`）**

| | |
|---|---|
| 十字键 | 逐行滚动（长按跳到顶/底） |
| L / R | 翻页 |
| × | 回到实时 |
| SELECT | 控制键面板 |
| ○ | 断开 |

**控制键面板**：Ctrl-C/D/Z/L/A/E/K/U/R/W、Esc、Tab、PgUp/PgDn、Home/End、
Ins/Del、F1–F12。选中后按 × 发送，面板不会自动关闭（控制键通常连着按）。

## 保存的密码

主机表单的密码行可以存下这台主机的密码，之后连接时不再弹输入框。

**怎么用**：表单里光标移到 `Password` 行，按 × 调出键盘输入，然后保存主机。
该行右侧的状态会实时告诉你当前会怎么处理：`(not saved)` / `(saved)` /
`(will be saved)` / `(will be forgotten)`。要**删掉**已存的密码，就在键盘里把它
清空再确认（进去时若已存过密码，键盘里会预填出来，这同时也是一种查看方式）。
非 ASCII 密码不会预填——系统输入法按字节映射，预填非 ASCII 会变乱码——这时
键盘是空的，直接回车表示保持不动。

存过密码的主机连接时不再弹输入框，直接用它登录。密码被服务器拒绝时**只自动
重试一次**，之后回落到输入框——否则一个过期的密码会被静默重试到连接断开，看起来
就是应用卡住了。

**存在哪**：`ux0:data/vitassh/secrets.dat`，定长 3668 字节。明文是一张 16 槽的
表，每槽 `host | user | port | password`；整张表用 **AES-256-GCM** 加密，密钥由
`PBKDF2-SHA256(本机 OpenPsId, 随机 salt, 10000 轮)` 导出，salt 和 GCM nonce
每次保存都重新取随机数。文件头（magic / salt / nonce）作为 AAD 一起认证，所以
改任何一个字节都会被发现。`strings secrets.dat` 里看不到任何密码。

**防住了什么**：内存卡单独被拿走、拷到电脑上，解不开——密钥在这台机器里，
从来没写进文件。

**没防住什么**：

- 任何能在这台机器上跑自制程序的人都能调 `sceKernelGetOpenPsId()`，因此也能
  解密。这层保护的对象是**卡**，不是**机器**。它**不是**主密码方案，也没有主
  密码——不要按「输一次密码解锁」的心智模型理解它。
- 卡和机器一起丢了，等于没加密。
- CFW（HENkaku/Enso）有可能把 OpenPsId 伪装成固定值，那样所有机器的密钥都一样，
  这层保护会明显变弱。应用内无法自证唯一性。能做的间接验证是：**冷启动一次，
  已存的密码仍然能自动登录**——说明这个值至少在重启之间是稳定的。

**匹配规则**：键是 `(host, user, port)`，**不含 name**。所以只改显示名不丢密码；
改了地址、端口或用户名，旧条目会被删掉（否则改几次就会把 16 个槽塞满垃圾）。
删主机连带删它的密码；两个主机如果完全同键，删其中一个不会动到另一个。

**坏文件**：长度不对、magic 不符、GCM 校验失败或槽内字符串没有终止符，都会被
改名成 `secrets.dat.bad`（只保留一代），然后当空库起步，日志里记一行。**如果
改名失败**（比如文件被占用），保存功能会整个关掉而不是覆盖它——坏文件是你唯一
的一份密码，覆盖掉就真没了。此时把它删掉即可恢复。

启动时会跑一遍已知答案自检（PBKDF2 与 AES-256-GCM 各一条定值，以及几条篡改必须
失败的用例）。不过就禁用保存密码并记下是第几条挂的：EVP 的参数传错不会报错，
只会生成一个自己验得过、别的什么都读不了的文件，这比直接失败更糟。

## 排错

### 安装报错

**`0x8010113D`（安装被拒）**：**不是** Title ID 冲突。那个说法是没有验证过的
猜测，已经被「换一个从未用过的 Title ID 依然报同一个错」直接证伪。这个错误码在
vitasdk 里查不到（模块半字节 `0x11`，没有任何已安装的头文件定义它），所以也不要
从它的字面含义去推理。

实际原因是 **LiveArea 资源**。定位它的方法：同一份 eboot 配上 SDK 自带
`hello_world` 的 `sce_sys/` 打包，就能装能跑；换回自己的资源就失败。eboot、
`param.sfo`、SELF 头、ZIP 结构以及各条打包命令行逐字段比对都相同，唯一的变量
就是那几张图。

自己的图和样例有两处不同，现在都对齐了：PNG 改成 **8 位索引色（颜色类型 3）**
——`hello_world` 的 icon0/bg/startup 都是索引色，而真彩色（颜色类型 2）是完全
合法的 PNG、Vita3K 也照收，真机安装器却会因为它拒掉整个 vpk；`template.xml`
与样例逐字节一致（`style="a1"`，不带 `<frame>`）。这两处到底哪一处致命没有单独
验证过。

两条走了弯路的教训：

- **Vita3K 会接受真机拒绝的 vpk**，所以「模拟器里能装」什么也证明不了。
- **失败之后重装同一个 vpk 会报出误导性的错误**：每次尝试都会在 `ux0:app/` 留下
  登记，于是重试报的是残留冲突而不是真正的故障。隔离问题时每次换一个新 ID。

`CMakeLists.txt` 里 `VITA_TITLEID` 和 `VITASSH_ASSETS` 都是 cache 变量，正是为了
这种对拍——换资源集或换 ID 都不用改源码：

```bash
cmake -DVITASSH_ASSETS="$VITASDK/share/gcc-arm-vita-eabi/samples/hello_world/sce_sys" \
      -DVITA_TITLEID=VSSH00003 -B build/probe -G Ninja ...
```

注意 `VITA_TITLEID` 是 cache 变量，改完要清掉构建目录或显式再传一次。

### `asking which authentication methods are offered failed`

密码和端口都填对，却弹 `Connection failed` 并把上面这句话当原因——这是上一版
的 bug，已修，值得记下来因为它极容易误判成密码错。

根因在 `userauth_list()` 的调用时机。libssh2 每完成一次
`libssh2_userauth_list()` 就把内部状态复位，所以**每个 tick 都重新查询一次认证
方式**，等于每一帧都发一个新的 "none" 探测请求。而 libssh2 取包的
`_libssh2_packet_requirev()` 会把任何匹配的包从队列里拿走，list 查询和密码尝试
等待的应答集合都含 `USERAUTH_SUCCESS`/`FAILURE`——于是那个多余的探测把本该交给
密码状态机的应答抢走了。

抢到 `USERAUTH_SUCCESS` 时，libssh2 把会话标记为已认证、错误码设为 0，然后
仍返回 `NULL`。老代码只看 `NULL` 就判定失败，**服务器其实已经点头了**。这也
解释了为什么错误信息里没有 libssh2 的细节：错误码为 0 时它的错误串是空的。

修法是把查询**锁成一次**（`Ssh.methods_asked`），并且在这一条 `NULL` 路径上先问
`libssh2_userauth_authenticated()`——那是纯位测试，无副作用。同一只 bug 还有
第二种表现：抢到的是密码尝试的 `FAILURE` 时，下次重入会读错应答，报「密码正确
却被拒 3 次」。

### 运行日志

vita2d 应用没有控制台，所以一切都写在 **`ux0:data/vitassh/log.txt`**。每行都是
独立的 `fopen`/`fwrite`/`fclose`，崩溃前写下的内容一定在盘上。日志超过 256 KB
会在下次启动时重建。

## 已知限制

- 只显示 ASCII。远端输出里的中文和 emoji 会显示成 `?`（收发链路本身是 UTF-8
  的，只是内嵌的点阵字体只有 ASCII 字形）。触摸键盘同样只发送 ASCII。
- 主机表单字段和登录密码仍走系统输入法（需要中文主机名或非 ASCII 密码时才有
  必要）；终端命令输入已经不需要离开终端画面。
- 只支持密码认证，没有公钥和 known_hosts 持久化 —— 每次连接都要确认指纹。
- 没有鼠标上报，`vim` 里用不了鼠标。
- 默认 MTU 下大文件传输受限于单线程非阻塞模型，`scp` 类用途没做。

## 目录

```
src/term.c vt.c config.c   终端核心，可宿主机测试（无 psp2 依赖）
src/vault.c                密码库：槽位、匹配、打包、原子写（同样可宿主机测试）
src/vault_platform_ossl.c  设备侧原语：libcrypto + OpenPsId + 随机数 + 自检
src/fileio.c               临时文件 + rename 的原子替换，配置与密码库共用
src/font8x8.c              内嵌 8x8 点阵字体（public domain）
src/net.c ssh.c            网络与 libssh2 非阻塞状态机
src/render.c ui.c ime.c    vita2d 绘制、界面状态机、系统输入法
src/keyboard.c touch.c    自绘触摸键盘与触摸屏事件
test/harness.c             483 项宿主机检查
test/vault_platform_stub.c 宿主机用的假原语（非加密，只为可测）
tools/hosttest.sh          宿主机测试构建脚本
tools/make_font.js         由 font8x8_basic.h 生成 font8x8.c
tools/make_assets.js       生成 sce_sys PNG（用同一套字体）
doc/HANDOFF.md             给改代码的人：构建命令、设计取舍、踩过的坑
```
