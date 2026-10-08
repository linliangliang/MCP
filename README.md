# weather-mcp：手写 C++ 的 MCP Server

用 C++ 从零实现的一个 MCP（Model Context Protocol）Server，暴露 `get_weather` 工具，
通过 [Open-Meteo](https://open-meteo.com/) 查询城市当前天气与未来 1-16 天预报（免 API key）。

**这是一个学习项目**：不依赖任何 MCP SDK（官方目前没有 C++ SDK），JSON-RPC、方法分派、
stdio 传输全部手写，目的是把协议本身看清楚。

---

## 一、它到底做了什么

CodeBuddy / MCP Inspector 以**子进程**方式拉起 `weather-mcp.exe`，双方通过 **stdin/stdout**
交换**换行分隔的单行 JSON**（注意：不是 LSP 那种 `Content-Length` 分帧）。

```
客户端 ──stdin──▶ {"jsonrpc":"2.0","id":1,"method":"tools/list"}
客户端 ◀─stdout── {"jsonrpc":"2.0","id":1,"result":{"tools":[...]}}
```

实现的 method：

| method                       | 说明                                                  |
| ---------------------------- | ----------------------------------------------------- |
| `initialize`                 | 握手，返回 `protocolVersion` / `capabilities` / `serverInfo`；`protocolVersion` 由 `params.protocolVersion` 协商得出 |
| `notifications/initialized`  | 通知（无 id），**不回响应**                             |
| `ping`                       | 心跳，返回空 result                                    |
| `tools/list`                 | 返回工具定义（name / description / inputSchema）        |
| `tools/call`                 | 执行工具，返回 `content` + `isError`                    |

---

## 二、构建

前置环境：Visual Studio 2022（含“使用 C++ 的桌面开发”）+ CMake ≥ 3.20。

```powershell
cd d:/Code/MCP

# 配置（只用一次）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# 编译
cmake --build build --config Release

# 产物
.\build\Release\weather-mcp.exe
```

没有安装 Ninja，默认用 Visual Studio 生成器；想更快可以 `winget install Ninja-build.Ninja`
后改用 `-G Ninja`。

### 依赖说明（刻意保持极薄）

| 依赖               | 引入方式                                | 理由 |
| ------------------ | --------------------------------------- | ---- |
| nlohmann/json 3.12 | 已在 `third_party/nlohmann/json.hpp`（单头文件） | 免包管理器，随仓库走 |
| HTTPS 客户端        | WinHTTP（系统自带）                      | 免 OpenSSL / vcpkg 编译 |

为什么不用 `vcpkg install nlohmann-json cpr`：HTTPS 在 Windows 上要带 OpenSSL，
源码编译一趟十几分钟起，还容易踩 runtime library（MT/MD）不一致的链接坑，
跟“学协议”这个目标抢精力。**代价是这份 HTTP 实现只能在 Windows 跑**——
所以它藏在一层极薄的接口后面（`include/mcp/http.hpp`），跨平台时只需再加一个
`src/http_curl.cpp`，`tools/weather.cpp` 一行都不用改：

```powershell
vcpkg install nlohmann-json cpr:x64-windows-static   # 可选的未来升级路线
```

---

## 三、先不用任何客户端也能验证

```powershell
# 协议冒烟：握手 / tools /list / tools/call / 错误语义，带断言
.\tests\smoke.ps1

# 只看工具真实输出（跳过断言）
.\tests\smoke.ps1 -Requests .\tests\weather-cases.txt -SkipAssert
```

`tests\smoke.ps1` 覆盖：

- `initialize` 返回协议版本
- 通知（`notifications/initialized`）不产生响应
- `tools/list` 能列出 `get_weather`
- `tools/call` 成功返回 `content`
- 未知工具 → JSON-RPC `-32602`；未知方法 → `-32601`
- 坏 JSON 行被跳过，**进程不退出**

> ⚠️ `tests\smoke.ps1` 必须保存为 **UTF-8 with BOM**。Windows PowerShell 5.1 默认按系统
> ANSI(GBK) 读取 `.ps1`，无 BOM 的中文注释会让脚本直接语法错。

---

## 四、接入 CodeBuddy

侧栏对话面板右上角 **CodeBuddy Settings** → **MCP** → **Add MCP**，写入：

```json
{
  "mcpServers": {
    "weather": {
      "type": "stdio",
      "command": "D:/Code/MCP/build/Release/weather-mcp.exe",
      "args": [],
      "description": "城市天气查询（Open-Meteo，免密钥）"
    }
  }
}
```

用户级配置文件就是 `%USERPROFILE%\.codebuddy\mcp.json`（本项目已写入）。

注意事项：

- Windows 路径用**正斜杠** `/`，避免 JSON 里的转义问题。
- 改完配置要**重启 MCP server 进程**（在 MCP 面板开关一下，或重启 CodeBuddy）。
- 面板显示**绿色**才算连上；红色时先看下一节的排错清单。
- 连上后在对话里直接问「杭州明天天气怎么样」，Agent 会自行调用工具。

---

## 四点五、配套的客户端 `mcp-client`

光有 server 就没法脱离 CodeBuddy 验证协议，所以补了一个最小的客户端：
它拉起 `weather-mcp.exe` 作**子进程**，在 stdio 上走完整的
`initialize → notifications/initialized → tools/list → tools/call`，
再把 `get_weather` 的结果写到本地文件。

```powershell
# 默认查杭州，结果写到当前目录 weather-result.txt
.\build\Release\mcp-client.exe

# 指定城市 / 天数 / 输出文件
.\build\Release\mcp-client.exe --city 武汉 --days 3 --out d:/tmp/wuhan.txt

# 写原始 JSON-RPC result 而不是纯文本
.\build\Release\mcp-client.exe --json
```

| 参数 | 默认值 | 说明 |
| ---- | ------ | ---- |
| `--exe` | 与 `mcp-client.exe` 同目录的 `weather-mcp.exe` | server 路径 |
| `--city` | `杭州` | 城市名，中文或英文 |
| `--days` | `2` | 预报天数，1-16 |
| `--out` | `weather-result.txt` | 结果文件 |
| `--json` | 关 | 落成 JSON 而非纯文本 |

退出码：`0` 成功，`1` 客户端/协议失败，`2` 工具返回 `isError=true`（文件照样写了）。

客户端这一侧同样有"握手成功但版本谈不拢"的坑，所以 `initialize` 之后会检查
server 回的版本是否在 `kSupportedProtocolVersions` 里，不在就直接报错，
而不是连上了再说。

---

---

## 四点六、Qt 界面客户端 `mcp-client-qt`（自然语言 → 工具 → 自然语言）

在 `mcp-client` 之上再加一层大模型，做成能聊天的窗口：

```
输入框："武汉明天天气怎么样"
   │
   ├─① DeepSeek 判断意图 → 决定调用 get_weather(city="武汉", days=2)
   ├─② 通过 McpClient 真的去问 weather-mcp（子进程，stdio JSON-RPC）
   ├─③ 工具结果作为 role="tool" 的消息回灌给 DeepSeek
   └─④ DeepSeek 组织成自然语言 → 追加到输出框
```

关键点：**模型只负责决策和措辞，数据一定来自 MCP server**，所以不会编造气温。

### 构建

Qt 是**可选依赖**，找不到就跳过这个 target，其余 target 照常构建：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      "-DCMAKE_PREFIX_PATH=C:/Qt/Qt5.12.12/5.12.12/msvc2015_64"
cmake --build build --config Release

# 启动（windeployqt 已把 Qt 运行时拷到 exe 同目录，可直接双击）
.\build\Release\mcp-client-qt.exe
```

### 用法

- 输入框写自然语言，`发送` 或 `Ctrl+Enter` 提交；输出框**逐轮往下追加**，不会覆盖上一轮。
- `清空对话` 只清历史，不断开 server。
- server 子进程只拉起一次，多轮复用；若中途退出了会自动重连一次。
- `--cli` 可不开窗口跑一轮自测，输出同样走终端：
  ```powershell
  .\build\Release\mcp-client-qt.exe --cli           # 跑内置的两轮示例
  .\build\Release\mcp-client-qt.exe --cli "上海今天天气"
  ```

### API key 放在本地配置文件里（不进版本库）

**源码里没有任何默认 key。** key 只从外部进来，取值顺序：

| 优先级 | 来源 | 说明 |
| --- | --- | --- |
| 1 | `--key <key>` | 命令行临时指定，覆盖一切 |
| 2 | `--config <路径>` | 显式指定的配置文件 |
| 3 | 环境变量 `DEEPSEEK_CONFIG` | 指向配置文件 |
| 4 | exe 同目录 / 当前目录的 `deepseek.local.json` | 默认位置 |
| 5 | 环境变量 `DEEPSEEK_API_KEY` | 没有配置文件时的退路 |

配置文件格式（模板见 `deepseek.example.json`）：

```json
{
  "apiKey": "sk-...",
  "model": "deepseek-chat"
}
```

仓库里**只提交** `deepseek.example.json`（模板），每人把它复制成 `deepseek.local.json`
再填自己的 key：

```powershell
Copy-Item deepseek.example.json deepseek.local.json
# 然后编辑 deepseek.local.json，填自己的 key
```

`.gitignore` 里已加 `deepseek.local.json` 和 `*.local.json`，可以直接确认：

```powershell
git check-ignore -v deepseek.local.json
# .gitignore:34:*.local.json    deepseek.local.json
```

启动时终端会打出 `使用配置：<来源>`，GUI 的连接提示里也会带上来源——
改了配置却没生效时，先看这一行就知道实际读的是哪份。

没配 key 时不会静默失败：CLI 打印创建方法后退出码 1，GUI 把同样的提示写进对话框并禁用发送按钮。

> 为什么不在源码里留默认值：写死默认值等于把密钥提交进版本库，push 到远端就收不回来了。
> 这个仓库目前还没有任何 commit，所以历史里是干净的；将来若不小心提交过 key，
> 正确的做法是在 DeepSeek 后台轮换（rotate），而不是只删文件——git 历史不会因为你删了文件而消失。

### 一个实测踩到的坑

第一版回答里模型把"明天"答成了今天（10 月 8 日）。原因不是工具数据错——工具返回的
明明是 `2026-10-08 / 2026-10-09` 两行——而是**模型不知道今天是几号**，只能猜。
修法是在 system prompt 里注入当天日期，并说明"预报第一行是今天，往下依次是明天"。
改完再问就是"武汉明天（10月9日）…16.8~26.2°C"，与工具数据一致。

---

## 五、排错清单（Windows 上的坑，基本都踩过一遍）

| 症状                                    | 原因与修法 |
| --------------------------------------- | ---------- |
| 客户端连不上，但程序单独跑没事           | **stdout 混进了非协议输出**。日志/调试必须走 `std::cerr`；一行 `printf` 都能让客户端判定握手失败 |
| 中文是乱码                               | 三处：① MSVC 编译必加 `/utf-8`（已在 CMakeLists 里）；② PowerShell 读文本文件要显式 `-Encoding UTF8`；③ `.ps1` 脚本自身要存成 UTF-8 **with BOM** |
| 终端里 `启动` 显示成 `鍚姩`                | **不是字节坏了**。程序输出的是 UTF-8，中文 Windows 控制台默认按 936(GBK) 解码。两个 exe 启动时都调了 `useUtf8Console()`（把控制台切到 65001）。若从 VS 调试启动后仍是乱码，确认改完有重新生成；**千万别改成"输出前转 GBK"**，那会真把数据写坏 |
| `invalid UTF-8 byte at index N` 只在 cmd 里传中文参数时出现 | `main(argc, argv)` 的 argv 是系统 ANSI(GBK)，塞进 JSON 请求体就炸。两个客户端都改用 `decodeCommandLineArgs()`：`GetCommandLineW` 拿 UTF-16 再转 UTF-8（见 `src/console_win.cpp`）。PowerShell 传参是 UTF-8 所以不复现，只在 cmd 下暴露 |
| 响应发出去了但客户端一直等                | 每个响应写完必须 `<< std::flush`（见 `transport_stdio.cpp`） |
| 某些字节处连接突然断开                    | Windows 文本模式下 `0x1A`(Ctrl-Z) 被当 EOF、`\n` 被翻译成 `\r\n`。启动即 `_setmode(_O_BINARY)` |
| HTTP 请求一直是乱码/404                   | URL 里的中文必须先 percent-encode（`urlEncode`）。WinHTTP 不会替你编码 |
| MCP 调用长时间卡住才开始报错              | 没设超时。`WinHttpSetTimeouts` 统一设成 10 秒 |
| 同名城市查错了地方                        | 见下：已按 `feature_code`（PPLC/PPLA…）+ population 优先选最知名的；只有候选完全没有排序依据时才把候选列出来让人确认 |
| 面板报 `Server's protocol version is not supported: X` | **server 没做版本协商**。客户端只认自己提过的版本，所以必须回它请求的那个版本（若我们支持），不能一律回自己的最新版。见 `negotiateProtocolVersion`；stderr 会打出 `requested=... -> negotiated=...`，对照一下就知道是谁不匹配 |

---

## 六、代码结构

```
CMakeLists.txt                  C++17 / MSVC 加 /utf-8 / 链接 winhttp.lib
third_party/nlohmann/json.hpp   JSON 库（vendored）
include/mcp/
  http.hpp                      唯一依赖平台的接口：httpGet(url, timeout)
  console.hpp                   控制台切 UTF-8 + 命令行参数按 UTF-8 取回
  process.hpp                   客户端侧的子进程 + 管道（PIMPL，挡住 windows.h）
  client.hpp                    McpClient：握手 / 通知 / tools.list / tools.call
  config.hpp                    LlmConfig：apiKey/model，来源本地配置文件
  deepseek.hpp                  DeepSeek /chat/completions + function calling
  agent.hpp                     WeatherAgent：模型决策 → MCP 调工具 → 结果回灌
  json_rpc.hpp                  makeResponse / makeError / 错误码常量
  transport_stdio.hpp           按行读写 stdin/stdout
  server.hpp                    ToolResult / ToolDef / ToolRegistry / handleRequest
  tools/weather.hpp
src/
  main.cpp                      _loop：逐行读 → parse → 分派 → 写回一行
  transport_stdio.cpp           Windows 二进制模式处理
  server.cpp                    method 分派与错误语义
  http_winhttp.cpp              WinHTTP 同步 GET
  console_win.cpp               SetConsoleOutputCP(CP_UTF8) + CommandLineToArgvW
  process_win.cpp               CreateProcess + 匿名管道（ChildProcess 实现）
  client.cpp                    McpClient：请求/响应匹配与版本协商检查
  client_main.cpp               客户端命令行入口，结果写文件
  config.cpp                    读本地配置文件拿 API key（不进版本库）
  agent.cpp                     tool-calling 循环（含工具失败重试、日期注入）
  deepseek.cpp                  大模型请求构造与响应解析
  qt_main.cpp                   Qt 窗口：输入框 + 输出框 + 异步问答
  tools/weather.cpp             业务逻辑：geocoding → forecast → 中文文本
tests/
  requests.txt                  协议冒烟报文
  weather-cases.txt             天气工具输入样例
  smoke.ps1                     冒烟脚本（含断言）
```

分层原则：**transport 只管字节，server 只管 JSON-RPC 语义，tools 只管业务**。
加第二个工具时照抄 `tools/weather.cpp` 即可，不需要改协议层。

---

## 七、错误语义：三类失败要分开

这是写 MCP server 最容易搞混的地方：

1. **报文不是合法 JSON** → `-32700`，跳过该行，**进程继续活着**
2. **method 不存在 / 参数不合法** → JSON-RPC error（`-32601` / `-32602`）
3. **工具执行失败**（城市找不到、网络超时） → **正常响应**，`content` 里写清原因 + `isError: true`

第 3 类不是协议错误。把它当成协议错误，模型就拿不到失败原因，也就无法自我纠正。

---

## 八、接下来的扩展方向

- **更多工具**：照抄 `tools/weather.cpp`，在 `registerWeatherTools` 里多 `registry.add(...)` 一次
- **resources / prompts**：在 `initialize` 的 `capabilities` 里声明，再实现对应 method
- **Streamable HTTP**：transport 换成 HTTP 服务端，JSON-RPC 层完全复用
- **换国内的天气源**（和风/高德）：只需改 `tools/weather.cpp` 里拼 URL 和解析的部分，
  key 通过环境变量读，在 CodeBuddy MCP 配置的 `env` 字段里注入
