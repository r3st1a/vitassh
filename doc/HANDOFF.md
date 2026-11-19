# VitaSSH 交接文档

写给下一个接手这个仓库的 agent。README 面向使用者，这份面向改代码的人：
**怎么构建、代码在哪、哪些决定不能随便动、哪些坑已经踩过。**
最后更新：2026-09-27。

---

## 1. 一分钟速览

PlayStation Vita 上的 SSH 客户端，vita2d 画界面，libssh2 做协议，非阻塞状态机
每帧推进一次。交付物是可安装的 `vitassh.vpk`。

| | |
|---|---|
| 交付物 | GitHub Releases 的 `vitassh-1.0.0.vpk`（= `build/app/vitassh.vpk`，1.26 MB）；`dist/` 不进 git |
| Title ID | `VSSH00004`（`CMakeLists.txt`）；排查安装问题时换新 ID，见 §6 |
| 版本号 | 发布 `1.0.0`（tag `v1.0.0`，资产 `vitassh-1.0.0.vpk`）；包内 APP_VER 仍为 `01.00`，因为 `vita_create_vpk` 只接受 `##.##` |
| 宿主机测试 | **483 项，全绿**（gcc 与 clang ASan/UBSan 各跑一遍） |
| 真机验证 | 1.0.0 已在 Vita3K（已装固件）端到端跑通；真机触摸手感仍待验证，见 §7 |

已经做完的两件事（最近一轮）：

1. **修掉登录必失败**——密码正确也会弹 `asking which authentication methods are offered failed`。
2. **加了可选的密码保存**，底层 AES-256-GCM 加密落盘，不明文。

---

## 2. 目录地图

```
src/
  main.c            入口 + 帧循环（60 Hz，sceDisplayWaitVblankStart 自己等）
  ui.c              屏幕/覆盖层/应用状态机，最大的一个文件
  ssh.c net.c       libssh2 与 socket 的非阻塞状态机
  term.c vt.c       终端模拟核心（VT100/ANSI 子集）
  config.c          hosts.txt 解析与组装
  vault.c           密码库：槽位、匹配、打包、原子写
  vault_platform.h  密码学原语的接口（见 §5）
  vault_platform_ossl.c  设备侧原语：libcrypto + OpenPsId + RNG + 已知答案自检
  fileio.c          临时文件 + rename 的原子替换，config 与 vault 共用
  render.c input.c ime.c clock.c log.c font8x8.c  外设与绘图
  keyboard.c touch.c 自绘触摸键盘与触摸屏事件（键盘按键直接映射为终端字节）
test/
  harness.c             宿主机测试主体
  vault_platform_stub.c 宿主机用的假原语（**不是加密**，只为可测）
tools/
  hosttest.sh       宿主机测试构建脚本
  make_font.js      由 font8x8_basic.h 生成 font8x8.c
  make_assets.js    生成 sce_sys 下的 PNG
```

**宿主机可测的只有**：`term.c` `vt.c` `config.c` `fileio.c` `vault.c`。
其余都直接或间接依赖 `psp2/`，`tools/hosttest.sh` 不编译它们——**加了新逻辑，
先想能不能放进这几个文件里**。`vault.c` 能测是因为原语被抽到了接口后面（§5）。
`keyboard.c` 的映射逻辑本身不碰 psp2（只依赖 `touch.h` 的结构体），host 侧用
桩函数补掉 `render_*`/`now_ms` 就能测按键输出；本轮用这样的临时程序验证过
Ctrl-C、Alt、Shift/Caps、翻页、连发和方向键，但它没有进 `hosttest.sh`（需要
链接 render 桩），改动键盘映射后记得再跑一次。

**终端几何**：`TERM_COLS 120`、`TERM_ROWS 51`、`CELL_W 8`、`CELL_H 10`；
8x8 字形按 1:1 绘制。触摸键盘占 `y=260..544`（`KB_Y`），键盘上方正好
`KB_Y / CELL_H = 26` 行。弹出时 `draw_terminal()` 选择窗口起始行
`clamp(term.cy - 13, 0, 25)`，让光标留在可见区中部；shell 在底部时就是
底对齐，vim 这类全屏程序的光标也不会被键盘盖住。

设备上的数据文件：

| 路径 | 内容 |
|---|---|
| `ux0:data/vitassh/hosts.txt` | 主机列表，`name\|host\|port\|user`，`#` 注释 |
| `ux0:data/vitassh/secrets.dat` | 保存的密码，密文，3668 字节定长 |
| `ux0:data/vitassh/log.txt` | 全部日志，每行独立 fopen/fwrite/fclose，>256 KB 重建 |

---

## 3. 构建与测试

`VITASDK` 必须是**反斜杠形式**（Windows 上 toolchain 文件是拼字符串找的）。
CMake ≥ 3.16 和 Ninja 都可以；没有时 `python -m pip install cmake ninja`：

```bash
export VITASDK='/path/to/vitasdk'   # Windows: 'C:\vitasdk'
CM=cmake
NJ=ninja
# pip 装了但不在 PATH 时：
# CM="$(python -c 'import cmake,os;print(os.path.join(cmake.CMAKE_BIN_DIR,"cmake.exe"))')"
# NJ="$(python -c 'import ninja,os;print(os.path.join(ninja.BIN_DIR,"ninja.exe"))')"

"$CM" -S . -B build/app -G Ninja -DCMAKE_MAKE_PROGRAM="$NJ" -DCMAKE_BUILD_TYPE=Release
"$CM" --build build/app
# 产物 build/app/vitassh.vpk
```

宿主机没有 gcc/clang 时，`python -m pip install ziglang` 拿 Zig 当 C 编译器；
`tools/hosttest.sh` 本身仍假定 gcc/clang 在 PATH：

```bash
ZIG="$(python -c 'import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),"zig.exe"))')"
"$ZIG" cc -target x86_64-windows-gnu -std=c11 -g -O1 -Wall -Wextra -Isrc \
  src/term.c src/vt.c src/config.c src/fileio.c src/vault.c \
  test/vault_platform_stub.c test/harness.c -o build/hosttest_zig.exe
./build/hosttest_zig.exe
```

只查语法（不需要链接，几秒）：

```bash
export VITASDK='/path/to/vitasdk'
for f in src/*.c; do "$VITASDK/bin/arm-vita-eabi-gcc" -std=gnu11 -Wall -Wextra \
  -Wno-unused-parameter -fsyntax-only -Isrc "$f"; done
```

**验收标准**：设备构建 `-Wall -Wextra` 零警告；宿主机两套测试都全绿。
提交前两样都要跑，宿主机测试很快（几秒）。

---

## 4. 架构要点

- **一切非阻塞**。`ssh_tick()` 每帧被 `ui.c` 调一次，内部是状态机
  （`SSH_CONNECT → HANDSHAKE → HOSTKEY → AUTH → CHANNEL → PTY → SHELL → RUNNING`），
  任何一步返回 `EAGAIN` 就保留状态等下帧。**不要在里面阻塞**，界面会卡住。
- **`App app` 是文件级 static 的**（`ui.c`），整个会话唯一。它很大（终端网格 +
  回滚环），所以放在静态区而不是栈上。同理，占空间的临时变量用 `static`。
- **每个 tick 的事件**通过 `SshEvent` 返回给 `ui.c`，`ui.c` 翻译成界面动作。
  `ssh.c` 不碰界面，`ui.c` 不碰 libssh2。
- **回滚/输入/渲染分离**：`vt.c` 解析字节流，`term.c` 维护网格，`ui.c` 只读网格。

---

## 5. 三个不能随便动的设计

### 5.1 认证状态机：`libssh2_userauth_list()` 只能调一次

**这是个已经修掉的 bug，别把它改回去。** libssh2 每完成一次
`libssh2_userauth_list()` 就把内部状态复位，所以「每个 tick 查一次认证方式」
等于每帧发一个新的 "none" 探测请求。而 libssh2 的取包函数会把任何匹配的包从
队列拿走——探测和密码尝试等待的应答集合（`USERAUTH_SUCCESS`/`FAILURE`）是重叠
的，于是探测把该给密码状态机的应答抢走。

抢到 `SUCCESS` 时 libssh2 已经标记会话为已认证、错误码置 0，却仍返回 `NULL`；
只看 `NULL` 判失败就会在服务器已经点头之后把连接掐掉。

现在的写法（`src/ssh.c` 的 `SSH_AUTH` 分支）：

- `Ssh.methods_asked` 把查询锁成一次，本连接内永不重发。
- `libssh2_userauth_authenticated()` 是纯位测试、无副作用，可以每帧调；在
  `NULL` 路径上**先于** `rc` 判断。
- 非 EAGAIN 的 `NULL` 一律终结，**不要加重试计数**。

### 5.2 密码库：接口分层是为了可测

宿主机没有 OpenSSL，所以密码学原语放在 `vault_platform.h` 这个接口后面：

- `src/vault_platform_ossl.c` —— 设备实现（libcrypto + `sceKernelGetOpenPsId` +
  `sceKernelGetRandomNumber` + 启动自检）。**这是整个功能的信任边界**，
  其他部分都是记账。
- `test/vault_platform_stub.c` —— 宿主机实现，确定性玩具密码，**明确不是加密**，
  只保证篡改能被发现，好让 `vault.c` 的逻辑测得动。

所以 `vault.c`（槽位、匹配、打包、原子写、损坏处理）能在 Windows 上跑完
483 项检查里的绝大部分。**改 `vault.c` 一定要跑宿主机测试**；
改 `vault_platform_ossl.c` 宿主机测不到，只能靠它自己的已知答案自检。

加密参数：`PBKDF2-SHA256(OpenPsId, salt, 10000 轮)` 导出 32 字节密钥，
AES-256-GCM 加密整张槽位表，文件头（magic‖salt‖nonce）作为 AAD 一起认证。
文件格式和威胁模型见 `src/vault.h` 顶部注释和 README「保存的密码」一节。

**选 GCM 而不是 CBC+HMAC 的理由**：没有补块运算，也就没有「3616 字节刚好是
16 的倍数、PKCS#7 还要再加一整块」这类尺寸错误；同时省掉第二个 MAC 密钥。

**迭代次数写在代码里而不是存进文件**：存了就得在验证之前读它，等于让一个坏文件
要求十亿轮 KDF 把启动拖死。改参数 = 换 magic 版本号。

**自检的常量必须来自独立实现**（`openssl` CLI），不能是本代码自己的输出——
自己验自己在密码学上没有意义。上一轮就差点把一个记错的 GCM 常量写进去
（把 AES-128 的 H 记成了 AES-256 的）。

### 5.3 原子写：`file_replace(tmp, path)`

`hosts.txt` 和 `secrets.dat` 都走 `fileio.c` 的「写临时文件 → rename」。
两个约束：

- **rename 之前必须先 `fclose`**。文件还开着时 rename 会失败——Windows 上如此，
  设备上也一样。这个 bug 被宿主机测试抓到过一次（`vault_load` 的损坏文件分支，
  表现为坏文件没法被移走、密码库整个不可用）。
- 保存失败时**不改内存状态**。`ui.c` 的 `form_commit()` 是在副本上操作、
  两个文件按密码库→主机的顺序写，失败路径的残留状态都是良性的。
  「两个文件不可能原子提交」是躲不掉的，只能保证顺序和报错说实话。

### 5.4 UI 的两个不变量

- **`form_forget_password()` 是关闭表单的唯一出口**。`app` 是 static，密码是明文
  存在里面的；`show_message()` / `back_to_hosts()` / 取消 / 提交完成都走这个函数。
  加了新的关闭路径要一起接上。
- **保存的密码每次连接只自动尝试一次**（`app.saved_tried`）。密码被拒后状态机
  会复位并再发同一个事件，天真的写法会把过期密码静默重试 3 次、永远不弹框——
  看起来就是应用卡死。

---

## 6. 踩过的坑（别再踩）

**真机安装**

- `0x8010113D`（安装被拒）**不是** Title ID 冲突，换 ID 复现过。真因是 LiveArea
  资源：PNG **必须是 8 位索引色（颜色类型 3）**。真彩色 PNG 完全合法、Vita3K 也
  照收，真机安装器却会因此拒掉整个 vpk。`template.xml` 要 `style="a1"` 不带
  `<frame>`。详见 README「安装报错」。
- **Vita3K 会接受真机拒绝的 vpk**，模拟器里能装什么也证明不了。
- 失败后重装同一个 vpk 报的是**残留冲突**，不是真正的故障。隔离问题时每次换新 ID。
- **绝不给 `vita_create_self` 传 `MEMSIZE`**。文档写的是「Normal app: 0」，
  正常应用声明了非零值会产出真机安装器拒绝的 eboot。
- `VITA_TITLEID` 和 `VITASSH_ASSETS` 都是 cache 变量，改完要清构建目录或显式再传。

**Vita3K**

- **非阻塞 `connect()` 返回 `EAGAIN`（11）**，不是真机的 `EINPROGRESS`（119）。
  `net.c` 现在把 `EAGAIN`/`EWOULDBLOCK` 也当成“连接进行中”；否则 Vita3K 里
  每次连接都会被当成失败立刻关掉。
- **必须先装固件**：`os0` 非空才能在 `-r` 后真正启动游戏，否则点“开始”模拟器
  直接退出。装完固件后 `<Vita3K>/Vita3K.exe -r VSSH00004` 可用。
- **用 `PostMessage` 发按键，不要用 `SendInput`/`keybd_event`**：后者在这台机器上
  完全不进 SDL，前者可以直接把 `X`/`Q`/`Enter` 送进模拟手柄。触摸要先按一次
  `T`（GUI 的 toggle-touch），再把鼠标按下/抬起当作前触摸屏。
- 本地联调用的无密码 SSH 回声服务器在 `build/testserver/sshserver.py`
  （paramiko，`check_auth_none`，端口 2222）。预置
  `ux0/data/vitassh/hosts.txt` 为 `local|127.0.0.1|2222|test` 后，按 X 连接、
  按 Q 接受主机密钥即可进入 shell；服务器会把收到的每个字节回显到终端。

**运行时**

- **`sceKernelGetOpenPsId()` 返回全零**是调用失败/被拦截的样子。全零 ID 派生出的
  密钥是所有出同样问题的机器共用的，所以代码里直接拒绝（`ready = false`），
  **不回落、不明文、绝不写文件**。
- **随机数失败就放弃保存**，不回落到固定值：可预测的 GCM nonce 是破解，不是弱点。
- **`RAND_bytes` 不能用**：主机版 libcrypto 在 Vita 上没有 `/dev/urandom`，
  播种路径没保证。用 `sceKernelGetRandomNumber()`，一次最多 64 字节。

**调试**

- **Vita3K 用 `-r` 启动会跑已安装的旧 eboot.bin**，重新构建不会生效。测之前先重装。

  ```bash
  "<Vita3K>/Vita3K.exe" build/app/vitassh.vpk   # 安装（跑起来后杀掉进程）
  "<Vita3K>/Vita3K.exe" -r VSSH00004            # 启动已安装的
  ```

  模拟器状态在 `%APPDATA%\Vita3K\Vita3K\ux0\`。Git Bash 里
  `tasklist /FI "..."` 的 `/FI` 会被路径转换搞坏，要用
  `MSYS_NO_PATHCONV=1 tasklist`。
- vita2d 应用没有控制台，`ux0:data/vitassh/log.txt` 是唯一线索。日志每行都是
  独立的 fopen/fwrite/fclose（**只有 fclose 的内容才会真的落盘**），
  崩溃前写下的内容一定在盘上。

---

## 7. 当前状态与下一步

**已验证**（1.0.0，2026-11-19）：

- 宿主机 483 项检查全绿（zig cc）；设备构建 `-Wall -Wextra` 零警告。
- 键盘映射 host 测试：普通字符、Ctrl-C、Alt、Shift/Caps、三页切换、长按重发、
  CSI/SS3 方向键。
- **Vita3K 端到端**（固件已装）：主机列表 → 连接 127.0.0.1:2222 → 主机密钥
  L 接受 → none 认证 → PTY `120x51` → shell。触摸键盘可弹出/隐藏、点击终端
  重开；字母/数字/符号三页均能输入并回显；Ctrl-C 服务端收到 `0x03`；
  Shift 双击 Caps、单次 Shift 一次性生效；方向键发送 3 字节序列；
  长按 Del 连续发 `0x7f`；Enter 正常；键盘弹出时终端按光标位置滚动，
  底对齐时光标紧贴键盘上沿。

**未验证**（宿主机测不到，只能真机）：

1. **真机上的实际观感/触感**——Vita3K 鼠标模拟触摸不等同于电容屏；8px 字体在
   5 寸屏上是否够读、按键是否好按，需要真机确认。
2. **真实密码 IME 路径**——Vita3K 里系统 IME 的回车/提交没打通（本轮用
   `none` 认证绕开），密码登录本身仍应在真机确认。
3. **真实加密路径**——宿主机跑的是 stub。真机应确认：存完密码后把
   `secrets.dat` 拷到电脑上 `strings` 看不到明文；**冷启动一次**后已存密码仍能
   自动登录（这同时是 OpenPsId 在重启之间稳定的间接验证）。
4. 故意存一个错密码再连：应**只静默尝试一次**然后弹框。
5. 用 VitaShell 改坏 `secrets.dat` 一个字节再启动：照常启动、日志有记录、坏文件
   被改名成 `.bad`、之后存密码仍正常。

README 末尾有完整的 10 条真机检查清单。

**已知限制**（不是 bug，是没做）：

- 只显示 ASCII——内嵌的 8x8 点阵字体只有 ASCII 字形，远程的中文/emoji 显示成 `?`
  （收发链路本身是 UTF-8 的）。
- 只支持密码认证，没有公钥、没有 `known_hosts` 持久化，每次连接都要确认指纹。
- 没有鼠标上报，`vim` 里用不了鼠标。
- 默认 MTU 下单线程非阻塞模型传输慢，`scp` 类用途没做。

**最近几轮的结论**（避免重复劳动）：

- 「终端核心可宿主机测试」这个分层是刻意的，新逻辑尽量往 `term/vt/config/`
  `fileio/vault` 里放。
- 宿主机的 `vault_platform_stub.c` 永远是玩具实现，不要试图在宿主机上引入真
  OpenSSL。
- `Vault.dirty` 目前没有生产代码读，只有测试在断言它的语义
  （修改置位、保存成功清除）。留着是给「退出时按需写盘」这类需求用的。
