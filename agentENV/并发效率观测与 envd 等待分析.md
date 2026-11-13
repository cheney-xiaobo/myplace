---
title: AgentENV 并发效率观测与 envd 等待分析
aliases:
  - AgentENV envd 等待
  - agentenv-startup-metrics
tags:
  - agentenv
  - performance
  - observability
  - envd
source: /Users/cheney/.dsh/bot/workspace/AgentENV-main @ 34cdc8098096726646853a18cec7ae143995dcef
created: 2026-11-07
updated: 2026-11-07
status: 静态源码分析，未做 Linux/KVM 实测
---

# AgentENV 并发效率观测与 envd 等待分析

> 回答两个问题：实际压测应观测哪些指标和数据；如果启动时延主要发生在 envd 等待，整个流程究竟在做什么。
>
> 本文基于源码 `34cdc8098096726646853a18cec7ae143995dcef`，是静态分析，不是实测报告。现有指标能否直接拆出 envd 健康等待，会在下文明确区分。
>
> 相关笔记：[[单机并发启动配置与架构分析]]、[[并发启动控制点分析]]、[[沙箱拉起全流程]]。

## 一、最关键的判断

**“envd 等待很长”只是阶段归因，不是根因。**

它可能表示：

- VM 已收到 resume，但 guest vCPU 没得到足够宿主机 CPU；
- 恢复后的 envd 工作集仍在缺页或等待远程 I/O；
- tap/netns/路由/防火墙导致健康请求无法到达；
- envd 尚未监听、崩溃或被重启；
- 大量并发健康请求相互竞争；
- 外层指标把健康成功后的 `/init`、账号解析和卷挂载也一起算进了“envd 阶段”。

快照恢复与冷启动必须分开解释：

- **快照恢复不会重新执行 tools-image `/init` 和 `pivot-init`。** Firecracker 恢复已有 CPU/内存/设备状态，然后 resume；envd 应当是快照捕获时已经存在的服务。
- **完整冷启动会真正经历 guest boot。** tools-image `/init` 挂载 proc/sys/cgroup 和用户 rootfs、准备附加盘并 `pivot_root`，随后 `pivot-init` 准备 `/run`、DNS、loopback 和 runsv，最后启动 envd，见 `tools-image/init:12-96`、`tools-image/pivot-init:46-172`。

因此，不能把快照恢复时的 envd 等待描述成“重新完整 reboot”。

## 二、envd 健康等待之前，host 已经完成了什么

快照恢复核心顺序见 `src/sandbox/firecracker/sandbox.rs:1962-2336`：

```text
取得快照运行配置
→ 准备 tools/rootfs/附加盘和 volume 槽位
→ 取得暖 FC；miss 时创建网络并启动 FC
→ 设置 egress 和可选 custom-extension start hook
→ 创建或复用内存快照 ublk，可选 startup-pack 预取
→ FC 冷路径等待 API socket
→ load_snapshot_file（VM 仍处于 loaded-but-paused）
→ 设置 MMDS、协调 disk rate limiter
→ resume
→ start_nowait 返回
→ 进入 envd 健康等待
```

关键边界：

- `load_snapshot_file` 和 `resume` 都发生在健康等待之前，见 `src/sandbox/firecracker/sandbox.rs:2273-2336`。
- FC 池命中只减少网络/FC 创建和 API socket 等待，不会跳过 rootfs、内存设备、load/resume。
- `start_nowait()` 返回后，orchestrator 注册 handle、持久化过渡状态，然后调用 `wait_for_ready()`，见 `src/orchestrator/service.rs:2433-2499`。

所以如果只测到“envd 健康等待长”，不能反推 `load_snapshot` 或 `resume` 一定很快；需要独立时间点确认。

## 三、健康等待循环本身在做什么

`EnvdInstance::wait_for_ready()` 是一个逐 sandbox 的 HTTP 健康轮询循环，见 `src/sandbox/envd.rs:118-160`：

1. 计算总剩余超时；
2. 对 envd 发起 `health_get`；
3. 单次探测硬编码最多 1 秒，见 `src/sandbox/envd.rs:26`；
4. 成功立即返回；
5. 连接错误、HTTP 错误或探测超时后继续；
6. 默认 sleep 3 ms，再进入下一次；
7. 总耗时达到 `envd.init_timeout_secs` 后失败。

还有两个容易忽略的事实：

- 共享 bootstrap client 设置了 `pool_max_idle_per_host(0)`，不保留可复用的旧 idle TCP 连接，防止地址跨 runtime generation 复用时误连旧 VM，见 `src/sandbox/envd.rs:28-35`。
- 当前没有全局健康探测 semaphore，也没有指数退避或 jitter。`envd.poll_ms` 只控制失败后的间隔，不限制同时有多少 sandbox 在轮询。

在 500 个同时未就绪的实例上，3 ms 只是 sleep 间隔，**不代表每个实例一定每 3 ms 发一次请求**：连接建立、1 秒探测超时、 Tokio/CPU 调度和网络都会影响实际频率。但它确实可能形成明显的健康探测压力。

## 四、健康成功后，外层 `wait_for_ready()` 还没有结束

`FirecrackerSandbox::wait_for_ready()` 的完整顺序是，见 `src/sandbox/firecracker/sandbox.rs:908-959`：

```text
envd health 首次成功
→ 通知 tools/memory/rootfs 后台下载可以继续
→ 调用 envd /init
→ 挂载本次启动新增的 guest drives/volumes
→ 返回 orchestrator
→ 持久化 Running、发布 proxy route
→ API 返回
```

### 1. 后台下载通知

健康成功后，代码依次通知 tools、memory 和 rootfs 设备释放后台下载。memory/rootfs 通知是 best-effort；它们不代表整个后台下载已经完成。

这意味着先完成健康检查的实例，可能在其他实例仍启动时开始后台下载，从而竞争网络和磁盘。不能把“健康已成功”理解为后续没有存储流量。

### 2. `/init` 不只是一个空返回

`/init` 请求携带：

- 当前时间；
- 环境变量；
- 默认工作目录；
- 默认用户；
- access token。

见 `src/sandbox/envd.rs:188-209`。

如果默认用户需要解析，代码会先以 root 调用一次 `/init`，再读取/解析账号信息，解析步骤有 30 秒超时，最后才发起正式 `/init`，见 `src/sandbox/envd.rs:163-185`。所以数字 UID 或 `user:group` 形式可能增加 guest 文件和命令操作。

### 3. 新卷挂载也在 Running 之前

快照已有盘的 mount state 随快照恢复；只有本次新提供的盘需要重新执行 guest 侧创建目录和 mount。多个附加盘按顺序处理，见 `src/sandbox/firecracker/sandbox.rs:949-1012`。

因此观测字段必须分开：

- `health_wait`；
- `envd_init`；
- `guest_mounts`；
- `persist_running`。

否则很容易把整段都误称为“envd 等待”。

## 五、怎样计算并发效率

### 1. 每个请求至少记录这些原始字段

```text
request_start
response_end
success
http_status
error_class
sandbox_id
template_id / image_ref
operation = warm | cold
cache_generation = first | primed
client_connect_timeout
client_total_timeout
```

保留每次请求的原始时间戳，不要只保存平均延迟。没有原始数据就无法重建整批 wall time、尾延迟和启动完成曲线。

### 2. 整批指标

设一批请求从最早发起到最后一个响应结束：

```text
T_batch = max(response_end) - min(request_start)
X_batch = successful_sandboxes / T_batch
```

其中：

- `T_batch`：整批 wall time；
- `X_batch`：整批有效就绪吞吐，单位 sandbox/s；
- 另报成功率、p50/p90/p95/p99/max 和错误分类。

为了观察并发扩展效率，可用单并发吞吐作基线：

```text
throughput_1 = 1 / T_single
throughput_N = successful_sandboxes / T_batch_N
efficiency_N = throughput_N / (N × throughput_1)
```

`efficiency_N` 越接近 1，说明增加并发越接近线性收益；越低说明排队、资源饱和或失败损失越明显。它不能替代 p99 和成功率，高吞吐但大量失败的轮次无效。

### 3. 完成曲线比单个平均值更有用

按 100 ms 或 1 s 桶统计：

```text
x 轴：相对本批最早 request_start 的时间
y 轴：累计 Running 数、当桶 Running 数、失败数
```

典型形态：

- 长时间没有实例就绪，随后集中完成：可能在共同资源、快照加载或调度处排队；
- 前期快速完成，后期长尾：重点看尾部实例的 envd、I/O、CPU 调度和资源耗尽；
- 完成速率随 Running 数增加而下降：检查后台下载、metrics 采集、网络和存储竞争；
- 大量客户端超时但服务端随后 Running：检查客户端超时和 cancellation-safe 后台任务，不要把客户端断连当服务端取消。

## 六、现有 Prometheus 指标能看什么

### 1. HTTP 和启动阶段

节点已有：

- `agentenv_http_request_duration_seconds`，见 `src/observability/prometheus.rs:27-34`；
- `agentenv_sandbox_stage_duration_seconds`；
- `agentenv_sandbox_stage_inflight`，见 `src/observability/prometheus.rs:58-113`。

常用 PromQL：

```promql
# 各启动阶段 p95
histogram_quantile(
  0.95,
  sum by (le, operation, stage) (
    rate(agentenv_sandbox_stage_duration_seconds_bucket[5m])
  )
)
```

```promql
# 各阶段实际在飞数
sum by (operation, stage) (agentenv_sandbox_stage_inflight)
```

```promql
# 各阶段每秒完成数，按状态拆分
sum by (operation, stage, status) (
  rate(agentenv_sandbox_stage_duration_seconds_count[5m])
)
```

```promql
# HTTP p99，按 route/status 分析前先看实际 label 集合
histogram_quantile(
  0.99,
  sum by (le, route, status) (
    rate(agentenv_http_request_duration_seconds_bucket[5m])
  )
)
```

需要注意：`create_warm/create_sandbox` 包裹的是 orchestrator 从整体启动到 Running 的调用，见 `src/api/impls/sandbox.rs:1016-1124`。**它不能单独拆出 FC resume、envd 健康、`/init` 和卷挂载。** 现有粒度适合定位大阶段，不足以证明根因。

### 2. ublk 与 OverlayBD

节点有 `agentenv_ublk_operation_duration_seconds`，指标名定义见 `src/sandbox/ublk/device.rs:23-25`。

ublk daemon 默认独立监听 `0.0.0.0:9103`，见 `storage/ublk-daemon/src/main.rs:50-51`。OverlayBD 远程 I/O 指标包括：

- `agentenv_overlaybd_remote_read_duration_seconds`；
- `agentenv_overlaybd_remote_read_bytes_total`；
- `agentenv_overlaybd_remote_metadata_duration_seconds`。

定义和 label 见 `storage/overlaybd/src/metrics.rs:116-155`。

示例：

```promql
# 远程读取 p95，按 operation/status 分组
histogram_quantile(
  0.95,
  sum by (le, operation, status) (
    rate(agentenv_overlaybd_remote_read_duration_seconds_bucket[5m])
  )
)
```

```promql
# 远程读取有效吞吐
sum by (operation) (
  rate(agentenv_overlaybd_remote_read_bytes_total[5m])
)
```

```promql
# 远程 metadata 错误/超时占比，先检查 status 实际取值
sum by (status) (
  rate(agentenv_overlaybd_remote_metadata_duration_seconds_count[5m])
)
```

### 3. 抓取两个 metrics 端点

将节点端口替换为实际配置；AgentENV 与 ublk daemon 不是同一个默认 metrics 端点：

```bash
curl -fsS 'http://127.0.0.1:<agentenv-metrics-port>/metrics' \
  > "agentenv-metrics-$(date +%Y%m%d-%H%M%S).txt"

curl -fsS 'http://127.0.0.1:9103/metrics' \
  > "ublk-metrics-$(date +%Y%m%d-%H%M%S).txt"
```

至少在压测前、整批完成时各抓一次；若 Prometheus 持续抓取，则直接用时间范围查询对齐压测窗口。

## 七、当前缺少的关键指标

基线中没有现成的 envd 健康等待 histogram，也没有三类 warm pool 的统一命中率指标。若要把“envd 很慢”定位到具体子阶段，建议为每次启动记录：

```text
t_start_nowait_begin
t_load_snapshot_return
t_resume_return
t_health_first_success
t_envd_init_return
t_guest_mounts_return
t_api_running
```

派生区间：

| 区间 | 回答的问题 |
| --- | --- |
| `start_nowait_begin → load_snapshot_return` | 设备、网络、FC 准备和快照 load 总共多久 |
| `load_snapshot_return → resume_return` | MMDS、rate limiter 和 resume 调用多久 |
| `resume_return → health_first_success` | 真正的 envd 健康等待多久 |
| `health_first_success → envd_init_return` | 后台下载通知、账号解析和 `/init` 多久 |
| `envd_init_return → guest_mounts_return` | 新卷 guest mount 多久 |
| `guest_mounts_return → api_running` | 状态持久化、proxy 和 API 返回多久 |

健康循环还应增加：

- 尝试次数；
- 首次成功所需时间；
- 连接拒绝、连接超时、HTTP 非成功、1 秒探测超时的分类计数；
- 总超时次数；
- 每次探测耗时 histogram；
- 当前健康等待中的 sandbox 数。

三类池至少记录 `acquire_hit`、`acquire_miss`、`cold_fallback`、当前 idle 数和后台补池在飞数。没有这些数据，就无法把“预热不足”和“主机已经饱和”分开。

## 八、envd 等待长的诊断矩阵

| 同期现象 | 更可能的方向 | 下一步核对 |
| --- | --- | --- |
| resume 到首次健康成功长，host run queue 高、CPU 饱和 | guest vCPU/宿主机调度饥饿 | 降并发对照；按物理核记录 CPU，比较 p50/p99 |
| 远程 read 延迟和字节数升高，iowait/块设备 await 高 | 恢复后工作集缺页、远程存储或本地 I/O 饱和 | 对齐远程 read、设备 await、startup-pack 开关和缓存冷热 |
| 大量连接拒绝，但稍后成功 | envd 尚未监听或正在重启 | guest serial/envd 日志、runsv 重启记录、冷启动 init 日志 |
| 大量 1 秒探测超时，CPU 也高 | 网络/request 排队或 guest/host 调度问题 | 降并发、增大 poll 间隔做 A/B，检查 tap/netns 和 conntrack |
| 健康已成功，但 `/init` 区间长 | 用户解析、guest 命令、envd 内部初始化 | 单独记录 `/init`，比较默认 root 与需解析 user |
| 只有带 volume 的请求尾延迟长 | 新卷设备或 guest mount | 单独记录 mount 阶段，基础组去掉附加盘 |
| 前期实例影响后期实例，后台下载流量上升 | 就绪后后台下载竞争 | 对 background download delay/jitter 做 A/B，不直接当作生产优化 |
| Running 越多，周期性 envd 请求越多 | guest metrics 背景负载 | 检查 metrics 周期，做 `metrics_interval_secs=0` 对照 |

周期 guest metrics 的并发为 `ceil(running_sandbox_count/5)`，500 Running 时最多 100，单次超时 200 ms；通过 `try_lock` 跳过正被生命周期操作占用的 sandbox，见 `src/orchestrator/sandbox_metrics.rs:20-25`、`src/orchestrator/sandbox_metrics.rs:162-211`。

## 九、建议的 A/B 顺序

1. **先补时间点和原始请求日志**，再调整参数；否则只能看到总时长变化。
2. 固定模板、缓存代次、资源规格和客户端超时，比较 1、10、50、100、200、300、500。
3. 将 `/sandboxes` 和 `/sandboxes/cold` 完全分开。
4. 对 envd `poll_ms` 比较 `3/10/20ms`，观察健康尝试数、CPU、首次成功 p50/p99，而不只看平均启动时延。
5. 做 guest metrics 开关对照，排除背景采样干扰。
6. 做 startup-pack 和后台下载开关/延迟对照，区分恢复缺页与后台流量。
7. 每次只改一个变量，并保留成功率、错误类别和整批 wall time。

**不建议先调大 `envd.init_timeout_secs`。** 它只延长失败容忍时间，不会加速 guest 恢复、网络连通或 envd 监听。延长超时只能把一部分“失败”变成“更晚成功”，可能让吞吐和尾延迟看起来暂时改善，却没有消除根因。
