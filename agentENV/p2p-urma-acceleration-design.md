# AgentENV P2P 框架 urma 加速层 — 设计文档

> 基于 master_main 代码现状（v0.2.5+）。P2P 框架已存在：`src/p2p/`（trait 抽象 + iroh 后端 + scheduler 发现），urma 加速作为**新增 transport 后端**接入。Mermaid 图可直接在 GitHub / VS Code 预览。

---

## 0. 现状盘点（设计基线）

```text
src/p2p/
├── transport.rs      P2pTransport trait（lookup/fetch/publish 抽象）
├── types.rs          P2pArtifactKey/Descriptor/Provider/FetchOptions
├── config.rs         P2pTransportKind::{Disabled, Iroh}  ← 扩展点
├── discovery/
│   ├── scheduler.rs  SchedulerPeerDiscovery（scheduler gRPC 发现）
│   └── mod.rs        Static/Noop 发现
├── iroh/             iroh-blobs 后端（现有唯一实传输后端）
└── mock.rs

src/overlaybd/p2p/    层身份（artifact.rs）+ cache + facade
services/             scheduler 存储 artifact→node hints（仅路由提示）
```

**现有约束（来自项目经验，设计必须解决）：**

| 已知问题 | 设计对策 |
| --- | --- |
| urma import_jetty 后首个 READ 可能硬件静默丢弃；冷启动窗口仅在目录查询路径有重试掩盖 | 读路径统一重试层（§4.1） |
| 单 peer catalog 串行查询，死节点十几秒拖尾（urma 发现链路修复后变显性） | 并发查询 + 超时预算（§4.2） |
| SegmentRegistry 256 段 LRU 仅 unregister，不同步 artifact 持有与 scheduler 记录 → 冷 artifact 幽灵源 | 逐出三同步协议（§4.3） |
| P2P 只加速访问，durable 记录在 repository；POSIX 不得从 peer 修复缺失文件 | 回退链终点始终是 repository（§3） |

---

## 1. 模块视图（urma 后端在 P2P 框架中的位置）

```mermaid
flowchart TB
    subgraph Consumers["消费方（不变）"]
        IMG["image cache service<br/>(src/image/cache/service.rs)"]
        OBDP2P["overlaybd p2p facade<br/>(src/overlaybd/p2p)"]
        SNAP["snapshot 发布/拉取<br/>(best-effort, repository commit 之后)"]
    end

    subgraph Framework["P2P 框架层（不变）"]
        TRAIT["P2pTransport trait<br/>lookup_with_hints / fetch / publish"]
        DISC["P2pPeerDiscovery<br/>(Scheduler/Static/Noop)"]
        TYPES["Artifact Key/Descriptor<br/>(内容寻址身份, 层 digest)"]
    end

    subgraph Backends["Transport 后端"]
        IROH["iroh-blobs 后端<br/>(现有, QUIC 传输)"]
        URMA["urma 后端 (新增)<br/>RDMA 直传"]
    end

    subgraph UrmaInternals["urma 后端内部"]
        JETTY["jetty/session 管理<br/>import_jetty / export_jetty"]
        SEG["SegmentRegistry<br/>(内存注册段 LRU, ≤256)"]
        RTR["读重试层<br/>(冷启动首读掩蔽)"]
        QRY["并发 catalog 查询器<br/>(fan-out + 每查超时)"]
        SYNC["逐出同步器<br/>(segment→artifact→scheduler)"]
    end

    subgraph Ctrl["控制面"]
        SCH["scheduler (Go)<br/>artifact→node hints"]
        REPO[("OSS repository<br/>(durable, 兜底)")]
    end

    IMG --> TRAIT
    OBDP2P --> TRAIT
    SNAP --> TRAIT
    TRAIT --> IROH
    TRAIT --> URMA
    URMA --> JETTY
    URMA --> SEG
    URMA --> RTR
    URMA --> QRY
    URMA --> SYNC
    URMA -. "hints 注册/查询" .-> SCH
    DISC -. "peer 发现" .-> URMA
    URMA -. "冷回退 (仅 OSS backend)" .-> REPO
    SYNC -. "逐出通知" .-> SCH
```

**接入方式**：`P2pTransportKind` 增加 `Urma` 变体（[src/p2p/mod.rs transport_from_config](file:///Users/cheney/work/code/AgentENV/src/p2p/mod.rs) 分支），或 `Urma` 作为 iroh 的**加速旁路**（hints 命中同网段 urma peer 时优先直传，miss 回落 iroh）——后者为推荐形态，两种后端互补而非互斥。

---

## 2. 进程视图

```mermaid
flowchart TB
    subgraph ServerNode["agentenv-server 进程（每节点）"]
        subgraph TokioRT["tokio runtime"]
            CONSUMERS["消费方任务<br/>(image cache / snapshot 发布)"]
            P2PAPI["P2P facade 任务"]
        end
        subgraph UrmaThreads["urma 后端线程域"]
            JETTYT["jetty 事件线程<br/>(urma 用户态驱动, 与 tokio 并桥)"]
            SEGT["SegmentRegistry<br/>(锁保护, 无独立线程)"]
        end
        RTRT["读重试 + 并发查询<br/>(tokio task, 超时包装)"]
    end

    subgraph PeerNode["对端 agentenv-server 进程"]
        EXPORT["export_jetty 服务<br/>(注册段读服务)"]
        CATALOG["catalog 应答<br/>(descriptor 查询)"]
    end

    subgraph GoSched["scheduler 进程 (Go)"]
        HINTS[("artifact→node hints<br/>(仅存储, 不建立绑定)")]
    end

    CONSUMERS --> P2PAPI
    P2PAPI --> RTRT
    RTRT --> JETTYT
    JETTYT -- "RDMA READ<br/>(数据面, 零拷贝)" --> EXPORT
    RTRT -- "catalog query<br/>(控制面)" --> CATALOG
    P2PAPI -. "hints 上报/查询" .-> GoSched
    SEGT --- JETTYT
```

**进程/线程要点：**

| 组件 | 位置 | 模型 |
| --- | --- | --- |
| P2P facade / 消费方 | agentenv-server tokio | 现有 async 任务，零改动 |
| urma jetty 事件驱动 | agentenv-server 专用线程 | urma 用户态verbs的完成事件线程；与 tokio 通过 channel 桥接（`spawn_blocking` / oneshot） |
| 读重试 + 并发查询 | tokio task | 包装层，无自有线程 |
| SegmentRegistry | 进程内共享结构 | `Mutex<HashMap>` + LRU；**逐出钩子同步触发**（§4.3） |
| export_jetty 服务 | 对端 agentenv-server | 每远端会话一个 jetty；内存注册段引用计数 |
| scheduler hints | Go 独立进程 | 现有 gRPC 接口复用，新增 urma 可达性标记 |

---

## 3. 数据流：fetch 的回退链（核心语义）

```text
P2pTransport::fetch(descriptor)
  ① descriptor.providers 里的 urma peer（hints 优先序）
     └─ RDMA READ 直传（零拷贝, 同网段 μs 级）
  ② urma 传输失败/超时 → iroh 后端（QUIC, 跨网段兜底）
  ③ iroh 也失败 → repository（OSS backend 直读）
     ⚠ POSIX backend 不从 peer 修复缺失文件（现有约束）
  publish: repository commit 完成 → 才上报 hints → urma export 注册（best-effort）
```

---

## 4. 关键时序图

### 4.1 fetch 加速路径（含冷启动重试）

```mermaid
sequenceDiagram
    autonumber
    participant C as 消费方<br/>(image cache)
    participant F as P2P facade
    participant U as urma 后端
    participant J as 本地 jetty<br/>(import)
    participant P as 对端 export_jetty
    participant I as iroh 后端
    participant O as OSS repository

    C->>F: fetch(descriptor)
    F->>U: providers 含 urma peer → 尝试直传

    rect rgb(255, 245, 230)
        Note over J,P: 会话建立（冷启动窗口）
        U->>J: import_jetty(peer)
        U->>P: 建立远端读域
        U->>J: READ(segment, offset, len)
        Note over J,P: ⚠ 首个 READ 可能静默丢弃<br/>(已知硬件行为)
        J--x U: 超时/无响应
        U->>U: 重试（固定次数+退避）<br/>——统一重试层, 不依赖上层感知
        U->>J: READ (retry)
        J-->>U: 数据 ✓
    end

    loop 剩余区间
        U->>J: READ
        J-->>U: 数据
    end
    U-->>F: 完整 artifact → destination
    F-->>C: size

    alt urma 路径整体失败
        U-->>F: Error(transport)
        F->>I: fetch (iroh 回退)
        alt iroh 失败
            I-->>F: Error
            F->>O: 直读 repository（终态兜底）
            O-->>F: 数据
        end
        F-->>C: size
    end
```

### 4.2 lookup 并发查询（消除死节点拖尾）

```mermaid
sequenceDiagram
    autonumber
    participant F as P2P facade
    participant Q as 并发查询器
    participant S as Scheduler hints
    participant P1 as Peer A (活)
    participant P2 as Peer B (死)
    participant P3 as Peer C (活)

    F->>Q: lookup_with_hints(key, hints)
    Q->>S: 查询 artifact→node (现有接口)
    S-->>Q: [A, B, C]

    par 每查询独立超时预算 (如 2s)
        Q->>P1: catalog query
        P1-->>Q: descriptor ✓
    and
        Q->>P2: catalog query
        Note over P2: 无响应…
        Q->>Q: 超时, 丢弃该 peer<br/>(不阻塞整体)
    and
        Q->>P3: catalog query
        P3-->>Q: descriptor ✓
    end

    Q-->>F: 合并 providers=[A, C]<br/>(hints 优先序排列)
    Note over Q: 整体延迟 = max(单查) ≈ 2s<br/>而非 Σ(串行) = 死节点拖尾十几秒
```

### 4.3 publish 与 SegmentRegistry 逐出同步（堵幽灵源）

```mermaid
sequenceDiagram
    autonumber
    participant SNAP as snapshot/层发布
    participant REPO as repository
    participant U as urma 后端
    participant SEG as SegmentRegistry<br/>(≤256 段 LRU)
    participant S as Scheduler hints

    rect rgb(230, 255, 230)
        Note over SNAP,S: publish（顺序是契约）
        SNAP->>REPO: commit artifact（durable 先行）
        REPO-->>SNAP: commit 成功
        SNAP->>U: publish (best-effort)
        U->>SEG: register(segment) ← LRU 插入
        U->>S: 上报 hints (digest→本节点, urma 可达)
    end

    rect rgb(255, 235, 235)
        Note over SEG,S: 逐出（三同步协议, 修复已知缺陷）
        SEG->>SEG: 段数 > 256 → LRU 选出 victim
        SEG->>SEG: ① unregister jetty（停止服务）
        SEG->>U: ② 清除本地 artifact 持有记录<br/>(本地 cache 层联动)
        U->>S: ③ 注销 hints（本节点不再持有）<br/>——修复"仅 unregister"的静默失联
        Note over S: ①②③ 完成前, victim 段对<br/>新查询不可见（先摘路由再释资源）
    end
```

### 4.4 端到端：快照发布 → 跨节点 P2P resume 加速

```mermaid
sequenceDiagram
    autonumber
    participant NA as 节点A (源)
    participant R as repository (OSS)
    participant S as Scheduler
    participant NB as 节点B (目的)

    NA->>R: snapshot commit (rootfs 层 + memfile)
    R-->>NA: committed
    NA->>NA: urma export 注册 + hints 上报

    Note over NB: 沙箱 resume 需要 digest
    NB->>S: lookup hints
    S-->>NB: [A (urma 同网段)]
    NB->>NA: urma 直传 fetch（4.1 流程）
    NA-->>NB: 数据 (RDMA, 绕过 OSS 带宽)
    Note over NB: 仅当 A 失联才回落 iroh/OSS

    alt 层被 A 逐出 (4.3 流程已同步)
        S-->>NB: hints 不含 A → 直接 OSS
        Note over NB: 不会出现"指向 A 但 A 无数据"的幽灵读
    end
```

---

## 5. 模块改动清单

| 位置 | 改动 | 量级 |
| --- | --- | --- |
| `src/p2p/config.rs` | `P2pTransportKind` + `Urma`（或 `IrohWithUrmaBypass`） | 小 |
| `src/p2p/urma/`（新增） | jetty 会话管理、SegmentRegistry（修三同步）、读重试层、并发查询器 | 大 |
| `src/p2p/iroh/transport.rs` | 回退链编排（urma miss → iroh → repository） | 中 |
| `src/overlaybd/p2p/artifact.rs` | 层身份复用（digest 既是 P2P key 又是 urma 段键） | 小 |
| `services/` scheduler | hints 表加 transport 可达性标记；逐出注销接口 | 小 |
| `src/cfg.rs` + `config/default.toml` | `[p2p.urma]` 段（段上限/查询超时/重试参数） | 小 |
| 内核 / pageout / ublk 数据面 | **零改动** | — |

## 6. 验证方案

```text
Phase 0  重试层单测：mock 首读丢弃 → 验证掩蔽与退避参数
Phase 1  双节点直传：层 fetch 延迟对比 (urma vs iroh vs OSS 直读)
         + 死节点注入：lookup 整体延迟 ≤ 单查超时（消除拖尾）
Phase 2  逐出协议：灌 300+ 段触发 LRU → 验证三同步后无幽灵 hints
Phase 3  回退链 E2E：kill urma 路径 → iroh 顶上 → kill iroh → OSS 兜底，
         全程 fetch 不失败
KPI: 同网段 fetch p99 相对 iroh 改善 ≥ 2x；
     publish→可发现 时延（hints 上报后）< 1s；
     死节点 lookup 拖尾从 ~10s+ 降到 ≤ 查询超时预算
```
