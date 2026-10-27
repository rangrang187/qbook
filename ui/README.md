# qbook-ui

只读监控页面通过 `C++ PUB → Node SUB → SSE → 浏览器` 接收近期指标；关闭页面或 Node 不改变订单簿处理。指标通道不补发历史。服务端订阅只在 Node 中创建，`zeromq` 原生模块不会进入浏览器代码。

## 固定依赖与安装

使用 Node.js **24.22.1 LTS**、npm **11.19.1**。`package.json` 固定直接依赖版本，`package-lock.json` 是唯一 npm 锁文件；从仓库的 `ui` 目录执行：

```bash
npm ci
npm run typecheck
npm run test:unit
```

`typecheck` 先用 `next typegen` 生成 Next 管理的路由类型，再执行严格的 `tsc --noEmit`，因此不要求预先保留上次 `.next` 构建目录。

主要版本为 Next.js 16.4.3、React/React DOM 19.3.1、TypeScript 7.0.2、zeromq 6.8.0、Vitest 5.0.5、Vite 8.3.5、Playwright 1.64.1。Node 24.22.1 满足它们的 engines 约束；TypeScript 7 使用 Next 的 CLI 类型检查后端，构建不跳过类型检查。`next.config.ts` 用 `serverExternalPackages: ['zeromq']` 保留原生加载。`allowScripts` 仅允许已固定 `zeromq@6.8.0` 的安装脚本验证/构建原生绑定；其余包不能由这一条许可执行任意安装脚本。

本次版本选择核对了官方 [Node 24.22.1 LTS 安全发布](https://nodejs.org/en/blog/release/v24.22.1)、[Next 2026-10-14 安全更新](https://nextjs.org/blog/nextjs-security-update-october-14-2026)、[React 官方更新](https://react.dev/blog)、[TypeScript 7 正式发布](https://devblogs.microsoft.com/typescript/announcing-typescript-7-0/)、[Vitest 5](https://vitest.dev/blog/vitest-5.html)、[Playwright 发布记录](https://playwright.dev/docs/release-notes) 和 [zeromq Subscriber 契约](https://zeromq.github.io/zeromq.js/classes/Subscriber.html)，并以 npm 官方 registry 元数据和安装后的实际程序核实版本。

本轮开始时开发容器没有独立 Node/npm。本轮从 [官方固定归档](https://nodejs.org/dist/v24.22.1/node-v24.22.1-linux-x64.tar.xz) 下载并按同版本 `SHASUMS256.txt` 校验 SHA-256：`6c172a9ab6718f7b801fb5a7bd486af2ee5e32758086a04b84fcce2aa77095e6`，随后正式安装到容器 `/opt/node-v24.22.1-linux-x64`，由 `/usr/local/bin/node`、`npm`、`npx`、`corepack` 链接提供命令。`build/v4-tools` 仅为本轮可清理的下载/探测目录；删除它不影响容器已安装的工具。现有 `cpp-dev:latest` 没有仓库内 Dockerfile 或固定 digest，这些容器内安装尚未成为可单独复现整个环境的镜像构建配置。

## 配置与运行

先按根 README 构建并运行完整 qbook。C++ 仍只读取工作目录的 `config/config.yaml`，其中 `metrics_endpoint` 与 `sample_period` 是指标发布配置。UI 的两个配置分别属于服务端和构建期：

| 配置 | 用途 | 未设置时 |
| --- | --- | --- |
| `QBOOK_METRICS_ENDPOINT` | Node 订阅端点，只接受服务端配置；浏览器不能指定地址 | `tcp://127.0.0.1:5556` |
| `NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS` | 页面预期采样周期，必须与本次 C++ `sample_period` 相同 | `100` |

空值或非法值明确报错，不静默回退默认值。TCP 端点须包含主机及 1～65535 端口，也支持绝对 IPC 路径。

```bash
# 从 ui/ 执行；C++ sample_period 同时配置为 100。
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=100 npm run build
QBOOK_METRICS_ENDPOINT=tcp://127.0.0.1:5556 npm start -- --hostname 0.0.0.0 --port 3000
```

在宿主机浏览器打开容器转发的 `http://localhost:3000`。通过 VS Code Ports 面板转发 3000 即可；指标端口只供 Node 访问，无须暴露给浏览器。Node 与 C++ 位于不同网络命名空间时，服务端配置必须指向 Node 可达的指标地址。仅 `npm start` 修改 `NEXT_PUBLIC_` 变量不会改变已生成的浏览器代码；周期改为 2000 ms 时需同步 C++ 并重新 `NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=2000 npm run build`。

## 精度、状态与口径

136 字节协议逐标量解码：时间戳是 int64，其余 16 项是 uint64；17 个 JSON 字段都用十进制字符串，浏览器完整校验后才恢复 BigInt。累计量先做准确差分，再检查能否转为安全绘图数值。

页面区分 connecting、live、stalled、offline；业务计数不变但持续收到样本仍是 live。默认 100 ms 周期对应 1 秒速率窗口和 1500 ms 停滞阈值；周期 2000 ms 对应 4 秒窗口和 6 秒阈值。速率按已覆盖的实际时间计算，首样本只建立基线；13 项累计量下降时重建基线，3 项 gauge 下降不视为重启。状态每 500 ms 检查，趋势按真实到达时间保留 60 秒、最多 2000 个绘图点；降采样只减少绘图点。定时器和缺样本时不制造零值点，断档和重连重建基线。

Feed/Writer 的 orders 包含撤单；本地逐笔挂单数在 L2 为 0；内存值是订单簿估计，不是 RSS；Writer 计数表示 Add 成功，不代表文件关闭或断电安全。抽样均值是 Book 内部应用耗时的近似观测，不是 P99 或端到端延迟。

每个 SSE 请求拥有一个 SUB 和一个接收任务；应用层最多一条已排队事件及一份最新待发样本，HTTP/操作系统缓冲不在此上界内。请求取消、读取取消和接收失败均清理订阅与监听，并结束接收任务。模块只共享一组 SIGINT/SIGTERM 停止通知及活动请求的清理回调，不共享订阅或样本；最后一个请求结束后移除进程监听。Next 生产服务停止时会等待 HTTP 响应结束，桥接在进程信号到达时中断 SSE、关闭 SUB，使长连接和慢客户端不能阻止 Node 退出。此时浏览器观察到 offline；核心 C++ 服务不受影响。

## 完整监控验收

协议与清理回归包含 C++ 六组固定样本、非法 DTO、慢读者只保留近期值、部分初始化失败、重复取消、原生 ZMQ 等待接收时关闭，以及 20 个独立请求只共用一组进程停止监听。真实 HTTP 验证还逐项核对了 17 个字段，并在 8 轮请求取消后观察 XPUB 订阅数回到 0；另在 4 个活跃 SSE 未先取消时验证 Node SIGINT/SIGTERM 能结束并释放全部订阅。这些 HTTP 结果不代替真实浏览器验收。

C++ 固定二进制与独立字符串期望共用根目录 `tests/fixtures/metrics/`。先完成根目录完整构建，生成 `build/debug/tests/runtime-paths.json`，再运行：

首次安装 Chromium 及其 Linux 运行库时，使用与锁文件相同的 Playwright 1.64.1：

```bash
# install-deps 需要容器系统包安装权限；不修改 C++ 固定依赖。
npx playwright install-deps chromium
PLAYWRIGHT_BROWSERS_PATH=../build/v4-tools/browser-cache npx playwright install chromium
```

本轮浏览器为 Chromium 156.0.8078.4；使用上述独立缓存，不清理用户全局浏览器缓存。浏览器下载成功不代表运行库已齐全，缺少系统库时需先完成官方 `install-deps`，再实际启动浏览器。以下测试命令同样从 `ui/` 执行，显式沿用安装时的浏览器缓存路径：

```bash
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=100 npm run build
PLAYWRIGHT_BROWSERS_PATH=../build/v4-tools/browser-cache \
  QBOOK_E2E_SAMPLE_PERIOD_MS=100 npm run test:e2e
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=2000 npm run build
PLAYWRIGHT_BROWSERS_PATH=../build/v4-tools/browser-cache \
  QBOOK_E2E_SAMPLE_PERIOD_MS=2000 npm run test:e2e
# 验收后恢复默认页面配置。
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=100 npm run build
```

编排器使用当前构建中的真实服务、固定发布器和文件读取器，启动已构建的 Next 生产服务并运行 Chromium；它不执行 UI 构建。每组必须先按配对周期重新 build，不能复用另一周期的浏览器代码。准确命令、浏览器版本和本轮实际结果记录于本地 `docs/releases/V4.md`。测试数据、浏览器下载、`node_modules`、`.next` 和构建输出不提交。
