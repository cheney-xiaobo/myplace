# P2P URMA 修复：composite 发现盲点（D）与 bounce 缓冲池（A）

状态：设计稿（待评审）
分支基线：`p2p-new`
前置调研：AgentENV urma transport 全量实读 + Mooncake ub transport 对照（本文档只引用已逐行核实的本仓代码）。

## 0. 目标与范围

| 项 | 内容 | 不做 |
| --- | --- | --- |
| D | 修复 composite 模式下 urma 腿的 scheduler 发现盲点（上报、存储、过滤三断点） | 不改 iroh 行为；不做 artifact→node 索引接入（D2 后续项） |
| D-R1（可选） | 发现时预热 peer 连接，把 UB 硬件 ~1s 冷启动窗口移出数据路径（含 hints 直连路径）；配套硬件验证项 V1 | 不阻塞 D 主链路 |
| A | bounce 缓冲池化：消除每 fetch 流的 alloc + `register_seg` churn，内存从"每并发流 ≤16MiB 无上界"变为"全局 N×4MiB 封顶" | 不做 READ 流水线（B 项，依赖 A）；不做 lease-backed Bytes 零拷贝（并入 B，见 2.7）；不改 `RmReader::read_range` 语义 |

两项互相独立，D 先行（半天级），A 随后（一天级）。R1 与 V1 随 D 联调一并处理。

---

## 1. D：composite 发现盲点

### 1.1 现状链路（三断点，全部已核实）

composite 模式下 urma 的远程目录查询（`lookup_with_hints` → scheduler peers → catalog.query）**恒为空集**，urma 只能靠调用方 hints 发现对端。链路上三个断点：

**断点 1 —— 上报：URMA 端点从未上报 scheduler。**

```
src/bin/server.rs:89            p2p_local_endpoint = p2p_transport.local_endpoint()
src/p2p/composite.rs:159-161    fn local_endpoint() -> self.fallback.local_endpoint()   // 只返回 iroh
src/observability/reporter.rs:63,101,273-282,382,431
                                心跳携带单个 p2p_endpoint（scheduler.proto HeartbeatRequest.p2p_endpoint = 9）
services/api/proto/scheduler.proto:170
                                P2pEndpoint p2p_endpoint = 9;                            // 单值字段
```

composite 的 `local_endpoint()` 直接代理到 fallback（iroh），urma 的 serve jetty 地址（`eid:uasid:jetty`，构造于 `src/p2p/urma/transport.rs:317-325`）**从不上报**。proto 字段本身是单值，也不支持同时报两个 backend。

**断点 2 —— 存储：Go registry 每节点只存一个端点。**

```
services/scheduler/internal/node_registry.go:40    record.p2pEndpoint *schedulerv1.P2PEndpoint   // 单值
services/scheduler/internal/node_registry.go:173   p2pEndpoint: cloneP2PEndpoint(req.GetP2PEndpoint())
services/scheduler/internal/node_registry.go:276-315  filterP2pPeers: 单端点与 backend 精确匹配（:305）
```

即使上报了两个端点，registry 也只按一个过滤。

**断点 3 —— 发现：两条腿的 discovery 都按 backend=="iroh" 过滤。**

```
src/p2p/mod.rs:100-133  peer_discovery_from_config 传 p2p.transport.backend_id()
src/p2p/config.rs:20-31 P2pTransportKind::backend_id() 对 Composite 恒返回 IROH_BACKEND_ID
src/p2p/mod.rs:72-90    composite 分支为 iroh 腿和 urma 腿各建一个 SchedulerPeerDiscovery，
                        但 backend 参数都来自同一个 backend_id() → 都过滤 "iroh"
src/p2p/urma/transport.rs:660  消费侧 filter(|p| p.endpoint.backend == URMA_BACKEND_ID) → 恒空
```

urma 腿的 discovery 从 scheduler 拿回的全是 iroh 端点，消费侧再按 urma 过滤一次 → 空集。

### 1.2 修复设计

#### 1.2.1 proto：心跳新增 repeated 端点字段（新字段号，wire 兼容）

```protobuf
// services/api/proto/scheduler.proto —— HeartbeatRequest
P2pEndpoint p2p_endpoint = 9;              // 保留不动（老字段，老 agent / 回退用）
repeated P2pEndpoint p2p_endpoints = 10;  // 新增：一个节点多个 backend 端点
```

不改字段 9 的类型（single→repeated 是 wire 不兼容）；新字段 10 对老 scheduler 是未知字段、天然忽略。
重新生成：`make -C services proto`（Go），Rust 侧经 `build.rs` 自动（AGENTS.md 生成代码表）。

#### 1.2.2 Go scheduler：多端点存储与过滤

`services/scheduler/internal/node_registry.go`：

```go
// record 结构（:40 一带）：单值改切片
type nodeRecord struct {
    ...
    p2pEndpoint  *schedulerv1.P2PEndpoint    // 兼容保留：字段 9 的值
    p2pEndpoints []*schedulerv1.P2PEndpoint  // 新增：字段 10 的值
}

// 心跳路径（:173 一带）：
//   优先取 req.GetP2pEndpoints()；非空则存入 p2pEndpoints，
//   同时把第一个 backend=="iroh" 的端点（若无则第一个）镜像进 p2pEndpoint 供回退。
//   为空则维持现状 GetP2pEndpoint()。

// filterP2pPeers（:276-315）：对每个 record，遍历 p2pEndpoints（为空则回退单值），
// 找 backend 与请求匹配且 address 非空的第一个端点；行为对单端点节点完全不变。
```

兼容矩阵（滚动升级安全）：

| agent | scheduler | 行为 |
| --- | --- | --- |
| 老（只发字段 9） | 新（读 10 回退 9） | 现状，无回归 |
| 新（发 10 + 镜像 9） | 老（忽略 10） | 现状，urma 发现仍靠 hints（可接受，升级完成即好） |
| 新 | 新 | 完整修复 |

#### 1.2.3 Rust：`local_endpoint()` → `local_endpoints()`

`P2pTransport` trait（`src/p2p/transport.rs:87`）：

```rust
/// Endpoints this node advertises for peer-to-peer artifact transfer,
/// one per enabled backend (composite returns two).
fn local_endpoints(&self) -> Vec<P2pEndpoint>;
```

四个实现 + 一处转发：

| 文件:行 | 现状 | 改为 |
| --- | --- | --- |
| `src/p2p/transport.rs:87-89`（默认实现） | `None` | `Vec::new()` |
| `src/p2p/iroh/transport.rs:693-695` | `Some(iroh)` | `vec![iroh]` |
| `src/p2p/urma/transport.rs:833-835` | `Some(urma)` | `vec![urma]` |
| `src/p2p/obs.rs:217-219`（转发） | 透传 | 透传 |
| `src/p2p/composite.rs:159-161` | fallback 单值 | **concat(primary 的 urma 端点, fallback 的 iroh 端点)，urma 在前** |

urma 在前的原因：新 scheduler 心跳镜像逻辑（1.2.2）取"第一个 iroh"写进老字段，与此顺序无关，但语义上 primary 优先更清晰。

`src/bin/server.rs:89` 与 `src/observability/reporter.rs`：

```rust
// server.rs:89
let p2p_local_endpoints = p2p_transport.local_endpoints();

// reporter.rs:48,63,101 一带：p2p_endpoint: Option<P2pEndpoint>
//   → p2p_endpoints: Vec<P2pEndpoint>
// build_heartbeat_request（:382,431 一带）：
//   新字段 p2p_endpoints：全量填入
//   老字段 p2p_endpoint：取第一个 backend=="iroh" 的端点（保持对老 scheduler 的现状语义）
```

#### 1.2.4 Rust：discovery 按腿传 backend

`src/p2p/mod.rs:100-133`，签名加显式 backend，废除 `backend_id()` 推断：

```rust
fn peer_discovery_from_config(
    config: &crate::cfg::AppConfig,
    p2p: &config::ResolvedP2pConfig,
    node_identity: &NodeIdentity,
    backend: Option<&'static str>,          // 新增：本腿要发现的 backend
) -> Arc<dyn P2pPeerDiscovery>
```

四个调用点：

| 位置（mod.rs） | 腿 | 传值 |
| --- | --- | --- |
| :39 | Iroh | `Some(IROH_BACKEND_ID)` |
| :48 | Urma（纯 urma 模式） | `Some(URMA_BACKEND_ID)` |
| :72-73 | Composite fallback | `Some(IROH_BACKEND_ID)` |
| :84-85 | Composite urma primary | `Some(URMA_BACKEND_ID)` |

`SchedulerPeerDiscovery::start`（`src/p2p/discovery/scheduler.rs:31-75`）**本体不改**——backend 参数本就是参数化的，只是调用方传错。`filter_scheduler_p2p_peers`（:208-225）与其单测（:241-264）保持不变。

`P2pTransportKind::backend_id()`（`src/p2p/config.rs:20-31`）：改后调用方只剩……核实后若无其他调用方则删除（含其上方 "Composite advertises iroh endpoints" 注释——该设计决策被本修复取代）。若静态 peers 路径或别处引用则保留给非 composite 分支用。

#### 1.2.5 D 可选小项：发现时预热连接（R1）

现状的 ~1s 硬件冷启动窗口（import 后首个操作被 UB 硬件静默丢弃）只在**目录查询路径**有 3×300ms 应用层重试掩盖（`transport.rs:661-685`）。正常流程 lookup 先于 READ，连接已被目录查询焐热；但 **hints 直连路径跳过目录查询，首个 READ 即为 import 后第一个操作**——若 READ 同样存在首包丢弃（代码注释只记录了 SEND 的行为，READ 未在硬件上验证），现状会耗满 30s `fetch_timeout` 而非快速重试。

D 修复后 scheduler 能稳定返回 urma peer 列表，可顺势把冷窗口移出数据路径。**实现方式：预热任务放在 urma transport 内部，轮询 discovery 共享的 peers 列表，而非给 discovery 模块加回调**——`peer_discovery.peers()` 只是 RwLock 读 + clone（`discovery/scheduler.rs:86-88`），无 RPC，零改动 discovery 模块；transport 在 `with_ops` 里已经同时持有 `connections` 与 `peer_discovery`（`transport.rs:302,286`），闭合无 chicken-and-egg。

**① `ctp.rs`：`PeerConnectionManager` 两处小改**

```rust
/// 非触碰式存在性检查：预热用。绝不调用 touch()——
/// 否则预热会不断续命连接，idle TTL 回收（C 项）永远不触发。
pub fn has_connection(&self, remote_eid: &str) -> bool {
    self.connections.lock().unwrap().contains_key(remote_eid)
}
```

`connect_sync`（`ctp.rs:73-122`）补**并发竞态双检**——预热任务与 fetch 流可能同时对同一新 peer 建联，当前"先检查后插入"窗口会双建 jetty，后插者覆盖前者、前者泄漏（unbind 永不执行）：

```rust
// connect_sync 末段，握手成功后（ctp.rs:113-121 现状直接 insert）：
let mut connections = self.connections.lock().unwrap();
if let Some(existing) = connections.get(&key) {
    // 并发对手已先插入：丢弃自己刚建的 jetty，返回缓存的那条
    drop(connections);
    let ops = self.context.ops();
    let _ = ops.unbind_jetty(jetty);
    let _ = ops.unimport_jetty(tjetty);
    let _ = ops.delete_jetty(jetty);
    // 注意：不 touch——避免给预热路径续命
    let connections = self.connections.lock().unwrap();
    let existing = connections.get(&key).expect("just checked");
    return Ok(existing.clone());
}
connections.insert(key, conn.clone());
Ok(conn)
```

**② `transport.rs`：预热任务（新增字段 + 构造时启动）**

```rust
pub(crate) struct UrmaP2pTransport {
    // ...现有字段...
    prewarm_task: Mutex<Option<tokio::task::AbortHandle>>,
}

// with_ops 末尾（:370-383 Self 构造前）：
let prewarm_task = if settings.prewarm_connections {
    let this = Arc::clone(&transport_arc);          // 构造后回填，见下
    Some(spawn_connection_prewarm(this, settings.prewarm_interval).abort_handle())
} else { None };
```

构造顺序注意：任务持有 `Arc<Self>` 而 `Self` 尚在构造——用两段式：先 `Arc::new(Self { prewarm_task: Mutex::new(None), ... })`，再 `let h = spawn_connection_prewarm(arc.clone(), interval); *arc.prewarm_task.lock() = Some(h)`。

```rust
async fn spawn_connection_prewarm(
    transport: Arc<UrmaP2pTransport>,
    interval: Duration,           // 默认 30s，见 ③
) {
    loop {
        tokio::time::sleep(interval).await;
        let peers = match transport.peer_discovery.peers().await {
            Ok(p) => p,
            Err(_) => continue,
        };
        for peer in peers.iter().filter(|p| p.endpoint.backend == URMA_BACKEND_ID) {
            // 解析 "eid-hex:uasid:jetty-id"（复用 catalog.rs:58 现有解析器）
            let Some(jetty_id) = jetty_id_from_address(&peer.endpoint.address) else {
                debug!(peer = %peer.node_id, "urma prewarm: unparseable endpoint");
                continue;
            };
            let eid_hex = jetty_id.eid.to_hex();
            // 关键：只补缺，不 touch 已有连接（否则 idle 回收永不触发）
            if transport.connections.has_connection(&eid_hex) {
                continue;
            }
            match transport.connections.ensure_connection(&jetty_id).await {
                Ok(_) => debug!(peer = %peer.node_id, "urma prewarm: connection established"),
                Err(err) => debug!(peer = %peer.node_id, error = %err, "urma prewarm: failed (non-fatal)"),
            }
        }
    }
}
```

`shutdown`（:837-857）追加：

```rust
if let Some(handle) = self.prewarm_task.lock().as_ref().map(|h| h.abort()) { /* 即上面 abort_handle 的调用 */ }
```

**③ 配置**（`src/cfg.rs` P2pUrmaConfig + `src/p2p/config.rs` UrmaSettings，与 2.5 同模式）：

```rust
/// Pre-establish URMA connections to discovered peers so the ~1s
/// hardware cold-start window stays off the data path.
#[config(default = true)]
pub prewarm_connections: bool,
/// Interval between prewarm sweeps. Default 30s.
#[config(default = 30u64)]
pub prewarm_interval_secs: u64,
```

轮询周期取 30s 而非 discovery 的 5s：预热只对"新增 peer"有意义，沙箱集群节点进出频率远低于 5s；30s 意味着新 peer 首个 fetch 仍可能自己付一次冷窗口（D 修复后）——可接受，后续按需调小。

**④ V1 硬件验证项（双节点联调时确认，非代码改动）**

目的：确认 import 后首个 READ 是否如 SEND 一样被静默丢弃（代码注释只记录了 SEND 行为，`transport.rs:662-665`）。

```text
步骤：
1. 节点 A publish 一个 512MiB artifact（保证 fetch 期足够长）
2. 节点 B 关闭预热（prewarm_connections=false），重启 daemon（清空连接缓存）
3. B 通过 hints 直连路径发起 fetch_byte_range（跳过目录查询），
   记录 t0=fetch 发起，t1=首 chunk 到达（obs 的 fetch/op 指标或 debug 日志时间戳）
4. 重复步骤 2-3 五次，取 t1-t0 分布
5. 对照组：B 先对同一 peer 做一次 lookup（焐热连接）再 fetch，同样记录

判读：
- t1-t0 ≈ 正常 RTT+读延迟（<50ms）→ 首个 READ 不丢弃，hints 路径无需额外处理，
  R1 仍建议开启（把冷窗口彻底移出首个用户可见请求）
- t1-t0 ≈ 30s（fetch_timeout 耗尽后重试成功）或系统性出现超时失败
  → 首个 READ 被丢弃且 fetch 内层无重试，R1 从"可选"升级为"必做"，
  并评估在 rm.rs post_read 错误分支加一次 invalidate 前快速重试
```

#### 1.2.6 R1 与 C（连接回收）的交互

预热只补缺、从不 touch（①的 `has_connection` 前置检查保证），因此与 idle TTL 回收**共存无冲突**：活跃 peer 被 fetch 侧 touch 保活，闲置 peer 照常被 reap。被回收的 peer 若仍在 scheduler 列表里，下一轮预热扫描（≤30s 后）会重建——重建即新 import，冷窗口随之回归；但该 peer 已闲置 ≥TTL(300s)，冷窗口发生在带外预热而非数据路径，无害。C 项落地时（reap_idle 接线）无需为 R1 做任何适配。

#### 1.2.7 D 的效果边界

- 修好后：composite 下 urma 腿的 `SchedulerPeerDiscovery` 过滤 backend=="urma"，scheduler 返回各节点的 urma 端点 → `lookup_with_hints` 的 M2 目录查询（`transport.rs:641-695`）恢复工作，hints 仍优先。
- **不做（D2，后续项）**：`peers_for_key`（scheduler artifact→node 端点索引）用于 urma lookup，避免全量轮询对端目录。前置条件是 urma publish 路径接 `discovery.record_key()`——当前 urma publish 是否调用 record_key 未核实，接入时一并处理。record_key/forget_key 的 backend 参数随 discovery 构造参数自动正确，无需额外改动。
- R1 预热为可选小项，不阻塞 D 主链路；V1 验证结论决定 hints 直连路径是否需要额外重试逻辑。

### 1.3 D 测试计划

1. Go 单测（`node_registry_test.go` 模式照抄）：心跳带两个端点 → `ListP2pPeers(backend="urma")` 返回 urma 端点、`backend="iroh"` 返回 iroh 端点、其它 backend 空。
2. Rust 单测：`local_endpoints()` 各实现（composite 返回 2 元有序）；reporter `build_heartbeat_request` 多端点 + 老字段镜像逻辑。
3. 集成（手工，双节点 UB 机器）：composite 模式 A/B 节点，B 不带 hints `lookup` A 发布的 key → 经 scheduler 发现走 urma 目录查询命中。现状此用例失败（这正是 `transport.rs:1005-1074` 两个 `#[ignore]` 测试想覆盖的场景之一，可顺带评估是否可解除 ignore——但 mock 完成路由问题是独立 bug，不随 D 修复）。

---

## 2. A：bounce 缓冲池

### 2.1 现状路径（已核实）

`remote_chunk_stream`（`src/p2p/urma/transport.rs:454-565`）每流：

```
window = min(len, MAX_BOUNCE_WINDOW=16MiB)                     // :42-43, :491
alloc 4K 对齐 + 清零                                            // :492-502
register_seg(window)                                            // :503-516
循环: read_range(pos, min(remaining, window))                  // :519-530
      （read_range 内部再按 chunk_bytes=4MiB 串行 WR，rm.rs:117-169）
      copy_from_slice → tx                                      // :548-550
unregister_seg + dealloc                                       // :561-562
```

问题：

1. **注册 churn**：每次 fetch（含 overlaybd 前台 range 读，`src/overlaybd/p2p/facade.rs:399` → `fetch_byte_range`）都完整走一遍 alloc/register/unregister/dealloc，成本随调用次数线性。
2. **内存无上界**：每并发流独立 ≤16MiB 注册缓冲，并发 fetch 数没有闸门——N 个并发 range 读就是 N×16MiB。
3. 小文件浪费：1MiB 的 manifest 也要走一次完整注册周期。

### 2.2 设计选型

| 方案 | 池元素 | 改动量 | 内存（默认） | 与 B（流水线）契合 |
| --- | --- | --- | --- | --- |
| A1 窗口池 | 16MiB（`MAX_BOUNCE_WINDOW`） | 最小（alloc 换租借） | pool_size×16MiB = 64MiB | 差（租期=整流） |
| **A2 chunk 池（选定）** | `read_chunk_bytes`=4MiB | 中（窗口循环重构为 chunk 循环） | pool_size×4MiB = 16MiB | 好（K 在途 = K 池元素） |

选 A2 的三个理由：内存上界小 4 倍且小文件不浪费；`RmReader::read_range` 的约束是"local buffer 覆盖 len"（`rm.rs:44`），单 chunk ≤ `chunk_bytes` 天然满足，无需动 rm.rs；流水线化（B）时 K 个在途 WR 恰好对应 K 个池租借，背压语义直接由池给出。

### 2.3 BouncePool 结构（新文件 `src/p2p/urma/bounce.rs`）

```rust
//! Pooled, pre-registered bounce buffers for one-sided READs.
use std::sync::Arc;
use tokio::sync::{Mutex, Semaphore};

use super::ops::{RegisteredSeg, TargetSegHandle, UrmaOps};
use super::context::UrmaContext;
use crate::p2p::error::{Error, Result};

pub(crate) struct BouncePool {
    context: Arc<UrmaContext>,
    chunk_bytes: usize,                 // = settings.read_chunk_bytes（4K 对齐钳制后）
    capacity: usize,                    // = settings.bounce_pool_size
    permits: Semaphore,                 // 并发租借上限 = capacity
    free: Mutex<Vec<BounceBuf>>,        // 已建可复用缓冲
    created: std::sync::atomic::AtomicUsize, // 惰性建满计数（观测用）
}

struct BounceBuf {
    addr: usize,                        // 4K 对齐分配基址
    layout: std::alloc::Layout,
    registered: RegisteredSeg,          // tseg + va + key
}

impl BouncePool {
    pub fn new(context: Arc<UrmaContext>, chunk_bytes: usize, capacity: usize) -> Self { ... }

    /// 租一个 chunk 缓冲。池满则等待（背压）。
    pub async fn acquire(&self) -> Result<LeasedBounce> {
        let permit = self.permits.acquire().await
            .map_err(|_| Error::Internal(anyhow!("urma bounce pool closed")))?;
        // fast path：free 栈弹出；慢路径在 permit 保护下惰性 alloc+register_seg
        ...
    }

    /// shutdown：注销全部已建缓冲（幂等）。
    pub fn shutdown(&self) { ... } // unregister_seg × created，best-effort
}

pub(crate) struct LeasedBounce {
    buf: BounceBuf,
    pool: Arc<BouncePool>,   // RAII：Drop 即归还（push 回 free + drop permit）
}
impl LeasedBounce {
    pub fn tseg(&self) -> TargetSegHandle { self.buf.registered.tseg }
    pub fn va(&self) -> u64 { self.buf.registered.va }
    pub fn chunk_bytes(&self) -> usize { self.pool.chunk_bytes }
    pub fn slice(&self, len: usize) -> &[u8] { ... } // 借给 copy_from_slice
}
```

要点：

- **惰性建满**：池不预热，首个 acquire 才 alloc+`register_seg`；建满后纯复用。注册次数从"每流 ≥1"降为"进程生命周期 ≤ capacity"。
- **RAII 归还**：consumer 提前 drop 流（`transport.rs:551` 的 dropped 路径）时 `LeasedBounce` Drop 自动归还，无泄漏路径。
- **无死锁**：每流同一时刻至多持 1 个租借（见 2.4 循环），不构成嵌套等待。
- 注册失败处理与现状一致（`transport.rs:503-516` 报错→流错误），不引入回退分配。

### 2.4 remote_chunk_stream 改造（`transport.rs:454-565`）

```rust
let mut remaining = len;
let mut pos = offset;
while remaining > 0 {
    let chunk_len = remaining.min(pool.chunk_bytes() as u64);
    let leased = match pool.acquire().await {
        Ok(l) => l,
        Err(e) => { /* record_stream("error", ...); tx Err; break; */ }
    };
    if let Err(err) = reader
        .read_range(&locator, pos, chunk_len, leased.tseg(), leased.va(), fetch_timeout)
        .await
    { /* 现状错误路径 */ }
    // obs/debug 日志保持
    let bounce_slice = leased.slice(chunk_len as usize);
    if tx.send(Ok(Bytes::copy_from_slice(bounce_slice))).await.is_err() {
        status = "dropped";
        record_stream(status, streamed, stream_start);
        break;                       // leased Drop 自动归还
    }
    pos += chunk_len;
    remaining -= chunk_len;
    streamed += chunk_len;
}   // 每轮结束 leased Drop 归还
```

差异对照：alloc/register（:491-516）与 unregister/dealloc（:561-562）整体删除，换成循环内 acquire/Drop；`MAX_BOUNCE_WINDOW` 常量（:42-43）**删除**（窗口概念退役，chunk 粒度取代）；`read_range` 内部 WR 切分逻辑（rm.rs:117-169，`chunk = remaining.min(self.chunk_bytes)`）在单 chunk≤chunk_bytes 输入下退化为单 WR，无需改。

`UrmaP2pTransport` 接线（`transport.rs:282-384` `with_ops`）：

```rust
let bounce_pool = Arc::new(BouncePool::new(
    context.clone(),
    settings.read_chunk_bytes.max(4096) as usize,
    settings.bounce_pool_size.max(1),
));
// Self { ..., bounce_pool, ... }
```

`shutdown`（`transport.rs:837-857`）加一行 `self.bounce_pool.shutdown();`（放在 `remote_cache.clear` 附近）。

### 2.5 配置

`src/cfg.rs` `P2pUrmaConfig`（:694-727，confique `#[config(default = ...)]` 模式）：

```rust
/// Number of pre-registered bounce buffers for one-sided READs.
/// Also bounds concurrent chunk reads across all fetch streams.
#[config(default = 4u64)]
pub bounce_pool_size: u64,
```

`src/p2p/config.rs` `UrmaSettings`（:35-67）加 `pub bounce_pool_size: usize`，`from_config`（:100-109）映射 `(p2p.urma.bounce_pool_size as usize).max(1)`。默认值与 `STREAM_CHANNEL_CAPACITY=4`（`transport.rs:44-45`，保留——它管的是 consumer 侧 channel 深度）数值巧合但语义不同，勿合并。

### 2.6 A 测试计划（全部 MockUrmaOps 可跑，无需硬件）

`src/p2p/urma/bounce.rs` 单测：

1. `register_seg` 恰好调用 `capacity` 次（MockUrmaOps 状态计数），N>capacity 并发 acquire 排队、归还后复用。
2. RAII：提前 drop `LeasedBounce` 后立即可再 acquire 同一缓冲（free 复用，无泄漏）。
3. `shutdown` 幂等且 unregister 次数 = created。

`transport.rs` 既有 fetch 单测适配：断言行为不变（fetch_bytes 拉全量、range 流语义），新增"并发 3 流 + pool=2 时 register_seg 总数 ≤2"断言。

### 2.7 数据链路拷贝分析与 B 阶段规划

A 保留一次中间拷贝，现状数据链路：

```
远端内存 —DMA→ bounce 池槽 —memcpy→ Bytes(堆) —write→ 文件页缓存
                        (copy_from_slice, 原 transport.rs:550)
```

**量化（为何不能永远留着）**：4MiB memcpy ≈ 160–400µs（单线程 10–25GB/s）；4MiB READ 在 UB 带宽（~25GB/s 级）同样 ≈160µs——串行循环中拷贝时间 ≈ 网络时间，占关键路径约 50%，1GiB artifact 额外 40–100ms CPU。A 阶段保留是为了简单正确；**B（READ 流水线）阶段必须一并消除**。

**B 阶段方案：lease-backed Bytes（选定，与流水线同一改动面）**

实现路径已核实：本仓 `bytes = 1.11.1`（Cargo.lock:1319-1320），`Bytes::from_owner`（bytes 1.10+ 的公开 API）可用——owner 的 `Drop` 即归还点，无需手搓 vtable。

**① `bounce.rs`：`LeasedBounce::into_bytes` + `BounceOwner`**

```rust
use bytes::{Bytes, Owner};

/// Bytes 的 owner：持有一个池槽。Bytes 全部克隆 drop 后本结构 drop，
/// 槽归还池。Mutex 包裹 permit 是为满足 Owner: Send + Sync
///（SemaphorePermit 为 Send；Sync 以 Mutex 补齐，实现时按编译器裁决为准）。
struct BounceOwner {
    slot: Option<BounceBuf>,                       // Drop 中 take，避免移动语义冲突
    permit: std::sync::Mutex<Option<tokio::sync::SemaphorePermit>>,
    pool: std::sync::Arc<BouncePool>,
    len: usize,                                    // 尾 chunk < chunk_bytes
}

// SAFETY 契约（Owner trait 要求）：
//  - as_bytes 每次返回同一 slice（这里恒为前 len 字节）✔
//  - 底层内存在 owner 存活期间不被修改 ✔——READ 完成后才调用 into_bytes；
//    池只会把 free 列表里的槽租出，绝不在租出期间写它
unsafe impl Owner for BounceOwner {
    fn as_bytes(&self) -> &[u8] {
        // SAFETY: slot 由 4K 对齐分配器分配、chunk_bytes 覆盖有效；
        // len ≤ chunk_bytes 在 into_bytes 入口断言
        unsafe { std::slice::from_raw_parts(self.slot.as_ref().unwrap().addr as *const u8, self.len) }
    }
}

impl Drop for BounceOwner {
    fn drop(&mut self) {
        // 顺序关键：先归还槽再释放 permit——唤醒的等待者
        // acquire 时槽必须已在 free 列表里，否则它会在 permit 保护下走
        // "惰性新建"分支多建一个（不死锁，但浪费容量）
        if let Some(slot) = self.slot.take() {
            self.pool.push_free(slot);
        }
        drop(self.permit.lock().unwrap().take());  // 释放容量
    }
}

impl LeasedBounce {
    /// 零拷贝交出 chunk。调用前提：READ 已完成、缓冲内容已就绪。
    pub fn into_bytes(self, len: usize) -> Bytes {
        assert!(len > 0 && len <= self.chunk_bytes(), "lease length out of range");
        let LeasedBounce { buf, permit, pool } = self;
        let owner = BounceOwner {
            slot: Some(buf),
            permit: std::sync::Mutex::new(Some(permit)),   // permit 随 buf 一起移交
            pool,
            len,
        };
        // SAFETY: 缓冲内容在 owner 存活期间不可变（见上方契约）
        unsafe { Bytes::from_owner(owner) }
    }
}
```

（`LeasedBounce` 现有定义需微调：`permit` 字段独立持有而非藏在 acquire 闭包里，`Drop for LeasedBounce` 保留——`into_bytes` 之外的路径仍走"drop 即还"。）

**② `remote_chunk_stream` 循环内替换（与 2.4 对照）**

```rust
// A 阶段（保留拷贝）：
let bounce_slice = leased.slice(chunk_len as usize);
if tx.send(Ok(Bytes::copy_from_slice(bounce_slice))).await.is_err() { ... }

// B 阶段（零拷贝）：拷贝消失；leased 的槽在 consumer drop 该 Bytes 时归还
let chunk = leased.into_bytes(chunk_len as usize);
if tx.send(Ok(chunk)).await.is_err() { ... }   // send 失败：Bytes 立即 drop → 槽归还
```

**③ B 阶段流水线循环（K 在途，改动面预告的具体化）**

```rust
// remote_chunk_stream 重构为窗口化；read_range 拆成 post + wait 两段（rm.rs 改动）
let k = pool.capacity().min(STREAM_CHANNEL_CAPACITY + 1);   // §并行度约束
let mut pending: VecDeque<(LeasedBounce, u64 /*offset*/)> = VecDeque::new();
let mut next_pos = offset;
let mut remaining = len;

// 填满窗口
while pending.len() < k && remaining > 0 {
    let chunk_len = remaining.min(pool.chunk_bytes() as u64);
    let leased = pool.acquire().await?;                       // 池即背压：池空则等待
    reader.post_read(&locator, next_pos, chunk_len, leased.tseg(), leased.va())?;
    pending.push_back((leased, next_pos));
    next_pos += chunk_len;
    remaining -= chunk_len;
}
// 排空一轮再补——completion 事件驱动（现 rm.rs 的 poll_cr 循环提为独立等待原语）
while let Some((mut leased, pos)) = pending.pop_front() {
    reader.wait_read_completion(&mut leased, fetch_timeout).await?;  // 轮询/睡眠由现 read_range 内部逻辑迁移
    let chunk_len = (pos - offset) /* ...实际为该槽对应的 chunk_len，需随 pending 携带 */;
    tx.send(Ok(leased.into_bytes(chunk_len))).await?;
    if remaining > 0 {
        let chunk_len = remaining.min(pool.chunk_bytes() as u64);
        let l = pool.acquire().await?;
        reader.post_read(&locator, next_pos, chunk_len, l.tseg(), l.va())?;
        pending.push_back((l, next_pos));
        next_pos += chunk_len;
        remaining -= chunk_len;
    }
}
```

（伪代码省略 dropped/error 路径——每条错误分支都要保证：pending 里的 LeasedBounce 显式 drop（或依赖 Drop），在途 WR 由 `wait_read_completion` 带超时兜底；实现时逐条核对。）

安全性论证：

- **租期逃逸**：`Bytes` 出 channel 后被 consumer 持有，槽归还延迟到 consumer drop——这正是零拷贝的代价与语义，可接受。
- **无死锁**：归还路径（consumer drop `Bytes` → owner Drop → Mutex push 回 free + drop permit）不等待任何池资源，不构成循环等待。producer 侧 `tx.send` 失败（consumer 已放弃流）时 `Bytes` 立即 drop 归还，也不阻塞。
- **线程安全**：`Owner: Send + Sync` 由 `Mutex<Option<SemaphorePermit>>` 与 `Arc` 字段满足；归还操作本身（Mutex push）线程安全。
- **租借粒度**：尾 chunk `len < chunk_bytes` 时 `as_bytes` 只暴露前 len 字节，槽整体归还，无部分租借状态。consumer 对该 `Bytes` 调 `split`/`slice` 派生的子视图全部 drop 后才触发归还。
- **不可变契约**：`from_owner` 要求底层字节在 owner 存活期间不可变——成立条件是"READ 完成后才能 into_bytes"，A/B 两阶段的循环顺序天然满足；需在 `into_bytes` 文档注释里写死这条调用前提。
- `fetch_bytes` 路径（`transport.rs:610-623` `extend_from_slice`）：立即拷贝进结果 Vec 后 `Bytes` 即 drop、槽即归还，不长期占用。
- **并行度约束**：B 阶段 K 个在途 WR 对应 K 个被 `Bytes` 持有的槽 + 至多 1 个正在 send 的槽，因此 `bounce_pool_size` 必须满足 `pool_size ≤ STREAM_CHANNEL_CAPACITY + 1` 的关系才不发生 producer 饥饿性等待；实现时以显式断言或配置校验表达（默认 4 ≤ 4+1 成立）。

**否决的替代方案**：

| 方案 | 否决理由 |
| --- | --- |
| Mooncake 式"注册目的地"（READ 直达目的地内存） | AgentENV 的目的地是每 fetch 不同的临时文件/Vec——按目的地注册会把 A 刚消除的注册 churn 加回来，只对 overlaybd 长期 cache 文件成立，跨层改动。零拷贝诉求由 lease-backed Bytes 以更小改动面达成。 |
| 永久接受拷贝 | 串行循环下占关键路径 ~50%，流水线化（B）后即使与在途 READ 重叠，仍浪费一半的 CPU/内存带宽——B 阶段没有理由保留。 |

**B（流水线）改动面预告**（详细设计另行出稿，此处仅锚定范围）：`remote_chunk_stream` 循环重构为 K 在途 WR（K=`bounce_pool_size`），acquire→post_read→入 pending map，completion 回来一个再 post 下一个；`rm.rs` 的 `read_range` 拆为 `post_read` + 等待 completion 两段（现签名一次一个 WR，拆开后才能流水线化）；obs 记录窗口指标。K 个在途恰好对应 K 个池租借，背压由池天然给出——这就是 A 选 chunk 池（A2）时预留的契合点。

### 2.8 内存账本（前后）

| 项 | 现状 | A 后 |
| --- | --- | --- |
| 注册 bounce 总量 | 每并发流 ≤16MiB，**无上界**（N 并发 = N×16MiB） | `pool_size × chunk_bytes` = 4×4MiB = **16MiB 封顶** |
| register_seg/unregister 次数 | 每 fetch 流 ≥1 对 | 进程生命周期 ≤ pool_size 次 |
| 小文件（如 1MiB manifest） | 全量注册周期 + 16MiB 窗口浪费（window=min(len,16MiB) 实为 1MiB，但注册开销照付） | 租一个 4MiB 槽，复用 |
| 消费端 Vec（fetch_bytes 全量收） | 不变（trait 注释已提示大件用 `fetch`） | 不变 |
| 页缓存 | 不变 | 不变 |

代价：池满时并发流在 acquire 排队（有界延迟换有界内存）。默认 4 并发 chunk 与当前典型 range 读并发（overlaybd 前台 32 并发是 layer 级,单 layer 流内部串行）不冲突；若实测排队成为瓶颈，调大 `bounce_pool_size` 即可。

---

## 3. 实施清单（按序）

| # | 改动 | 文件 | 验证 |
| --- | --- | --- | --- |
| D1 | proto 字段 10 + 再生成 | `services/api/proto/scheduler.proto`，`make -C services proto` | Go/Rust 编译 |
| D2 | registry 多端点存储/过滤 + 回退 | `node_registry.go:40,173,276-315` | Go 单测（1.3.1） |
| D3 | trait `local_endpoints()` + 5 处实现 | `src/p2p/transport.rs:87`、iroh:693、urma:833、obs:217、composite:159 | Rust 单测 |
| D4 | reporter/server 多端点 + 老字段镜像 | `src/bin/server.rs:89`、`reporter.rs:48-131,382-431` | 心跳单测 |
| D5 | discovery 按腿传 backend，删 backend_id()（若确认无他用） | `src/p2p/mod.rs:39-133`、`config.rs:20-31` | cargo test -p agentenv --lib p2p |
| R1a | `has_connection` + connect 竞态双检 | `src/p2p/urma/ctp.rs:73-122` | ctp 单测新增：并发 connect 不双建 jetty（MockUrmaOps 计数） |
| R1b | 预热任务 + shutdown abort | `src/p2p/urma/transport.rs:282-384,837-857` | mock：预热只对缺的 EID 建、不 touch 已有 |
| R1c | 配置两项 | `src/cfg.rs`、`src/p2p/config.rs` | make fmt / clippy |
| A1 | `BouncePool` 新文件 | `src/p2p/urma/bounce.rs` | 2.6 单测 |
| A2 | remote_chunk_stream 改造 + 常量删除 | `src/p2p/urma/transport.rs:42-45,454-565,837-857` | 既有 fetch 单测 + 新断言 |
| A3 | 配置两处 | `src/cfg.rs:694-727`、`src/p2p/config.rs:35-67` | `make fmt` / `make clippy` |

仓库级检查：`make fmt && make clippy`；`make -C services test`（D 涉及 Go）；`cargo test -p agentenv --lib`（p2p 模块单测全绿）。urma 集成验证需 UB 硬件（鲲鹏 + openEuler），在双节点环境按 1.3.3 手工执行。

## 4. 风险与不做的事

- proto 改动走新字段号，滚动升级两端均为现状行为，无窗口丢功能（见 1.2.2 矩阵）。
- A 不解决：READ 串行吞吐（B 项）、`fetch_bytes` 全量 Vec、发布侧整件拷贝（mmap 失败路径）、**bounce→Bytes 中间 memcpy（B 阶段以 lease-backed Bytes 消除，见 2.7）**。
- `transport.rs:1005-1074` 两个 `#[ignore]` 测试的 MockUrmaOps 完成路由缺陷与本两项无关，不承诺随手修复。
- 2 个既有行为不兼容点提醒：composite `local_endpoint()` 语义从"返回 fallback"变为"返回两个"——外部若有依赖单端点语义的调用点（仅 server.rs:89 一处，已列入 D4），无其他。
- R1 连接预热与 C（连接回收，独立后续项）存在交互：回收后重建 = 新 import = 冷窗口回归。组合落地时二选一：回收后重新预热，或 READ 失败快速重试（V1 验证结论决定）。C 未落地前 R1 独立成立。
- V1 为硬件验证项（首个 READ 是否被静默丢弃），非代码改动；其结论只影响 hints 直连路径是否需要额外重试，不影响 D/A 主体的正确性。
