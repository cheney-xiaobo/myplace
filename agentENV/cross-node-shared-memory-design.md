# AgentENV rootfs / memfile 跨节点共享 — 开发者视图设计

> 基于 master_main 代码现状（v0.2.5+）的设计文档。Mermaid 图可直接在 GitHub / VS Code 预览。

---

## 1. 现状：单节点内的共享结构（开发者组件视图）

```mermaid
flowchart TB
    subgraph Node["Host 节点（单节点视角）"]
        subgraph FC["Firecracker 进程 ×N（每沙箱一个）"]
            GRAM["guest RAM<br/>(匿名 mmap, uffd 注册)"]
        end

        subgraph DAEMON["ublk-daemon (per-node)"]
            UBLK["ublk 设备<br/>UVMUblkTarget"]
            OBD["overlaybd ImageFile<br/>(rootfs 层栈 + memfile)"]
        end

        PC[("内核 page cache<br/>(按 inode 全局共享)")]
        FFC[("full_file_cache<br/>(远端层本地缓存, 应用层 LRU)")]

        UFFD["uffd handler<br/>(storage/uffd-core)"]
        P2PC["P2P client<br/>(src/overlaybd/p2p)"]

        GRAM -- "UFFDIO_COPY<br/>缺页填充" --> UFFD
        UFFD -- "读 memfile" --> PC
        OBD -- "buffered read" --> PC
        OBD -- "未命中本地" --> FFC
        FFC -- "下载缺失层" --> REPO
        P2PC -- "加速分发" --> REPO
    end

    subgraph REPO["共享存储层（跨节点，已存在）"]
        OSS[("OSS repository<br/>managed-layers/{digest}<br/>rootfs 层 + memory snapshot")]
        REG[("OCI registry<br/>(外部层)")]
    end

    subgraph SCHED["Scheduler (Go, services/)"]
        HINT["artifact→node hints<br/>(P2P 分发路由)"]
    end

    REPO -. "内容寻址, 全局唯一" .-> Node
    HINT -. "查询/注册" .- P2PC
```

**现状共享语义小结：**

| 资源 | 节点内共享 | 跨节点共享 |
| --- | --- | --- |
| rootfs 不可变层 | page cache 按 inode 共享（同节点多沙箱一份） | **已实现**：内容寻址 + OSS 全局唯一 + P2P 加速分发 |
| memfile（memory snapshot） | page cache 按 inode 共享（uffd COPY 源） | **仅数据可达**（artifact 在 OSS）；热路径无共享 —— resume 必须 uffd COPY，副本每节点一份 |
| 可写 upper 层 | 不共享（沙箱私有） | 不应共享（per-sandbox 状态） |

**核心缺口**：memfile 的跨节点共享只有"下载整个文件"一种形态。节点 B 上的沙箱 resume 时，即使节点 A 的内存里已经有这份 memfile（page cache / 常驻），B 也必须从 OSS 拉全量或本地缓存副本。

---

## 1.5 进程视图（跨节点共享落地后的进程/线程布局）

```mermaid
flowchart TB
    subgraph NodeB["节点 B — agentenv-server 进程（新增组件归属）"]
        API["API 层 (axum, src/api)"]
        ORCH["orchestrator service<br/>(生命周期驱动)"]
        SBOX["sandbox 任务<br/>(tokio runtime)"]
        UFFDT["uffd handler 线程 ×k<br/>(Source trait: Local/Remote/Oss)"]
        URMAI["urma import 会话<br/>(每 digest 一个连接池)"]
        PROMT["promote worker<br/>(热页落地 full_file_cache)"]
        P2PB["P2P client (现有)"]
        PAGEOUT["pageout worker ×8<br/>(vPause, 零改动)"]
    end

    subgraph FCB2["节点 B — Firecracker 进程"]
        GRAMB2["guest RAM (uffd 注册)"]
    end

    subgraph DAEMONB["节点 B — ublk-daemon 进程"]
        OBD2["overlaybd targets<br/>(rootfs 层栈)"]
    end

    subgraph NodeA2["节点 A — agentenv-server 进程"]
        UFFDTA["uffd handler (本节点沙箱)"]
        URMAE["urma export 端点<br/>(新增: memfile 区间注册<br/>+ 每客户端读服务线程)"]
        SEGREG["SegmentRegistry<br/>(扩: 逐出同步 locator)"]
    end

    subgraph CTRL["控制面"]
        SCHD["scheduler (Go 独立进程)"]
        LOC2["memfile locator 表<br/>(digest → [节点], 心跳维护)"]
        REDIS[("Redis")]
        SCHD --- LOC2
        REDIS --- SCHD
    end

    GRAMB2 -- "UFFDIO_COPY 缺页" --> UFFDT
    UFFDT --> URMAI
    UFFDT --> PROMT
    SBOX --> OBD2
    ORCH --> API
    URMAI <== "RDMA READ" ==> URMAE
    UFFDTA --- URMAE
    SEGREG -. "逐出/注册同步" .-> LOC2
    URMAI -. "locate 查询" .-> LOC2
    P2PB -. "artifact hints (现有)" .-> SCHD
```

**进程归属要点：**

| 组件 | 进程 | 线程模型 |
| --- | --- | --- |
| uffd handler（Source trait 三实现） | agentenv-server | 缺页服务线程 ×k（现有模式扩展），**不进 ublk-daemon**——memfile 读源与块设备数据面解耦 |
| urma export | agentenv-server（源节点） | 每远端客户端一个服务线程；内存区间注册引用计数 |
| urma import | agentenv-server（目的节点） | 连接池，与 uffd 服务线程共享读域 |
| promote worker | agentenv-server | 复用 full_file_cache 写入路径（缓冲写 + 300s 批量刷盘语义不变） |
| memfile locator | scheduler（Go） | 心跳上报 + 查询；**只读路由，不建立绑定**（与现有 scheduler 语义一致） |
| pageout / vPause | agentenv-server | 现有 8 worker，零改动 |

---

## 2. 目标：跨节点共享架构（开发者视图）

设计原则：**rootfs 顺着既有 P2P 体系走（只补路由策略）；memfile 增加"远端直读"路径（urma/RDMA hints），避免落地副本。**

```mermaid
flowchart TB
    subgraph NodeA["节点 A（源节点：memfile 已驻留）"]
        PCA[("page cache / 常驻<br/>memfile 内容")]
        UFFDA["uffd handler<br/>(已在服务本节点沙箱)"]
        URMAA["urma export endpoint<br/>(新增：内存区域注册 + hints 服务)"]
        PCA --- UFFDA
        UFFDA --- URMAA
    end

    subgraph NodeB["节点 B（目的节点：新沙箱 resume）"]
        subgraph FCB["Firecracker 进程"]
            GRAMB["guest RAM<br/>(uffd 注册)"]
        end
        UFFDB["uffd handler<br/>(扩展：hints 直连分支)"]
        PCBL[("本地 page cache<br/>(仅热页 promote 落地)")]
        GRAMB -- "缺页" --> UFFDB
        UFFDB -- "命中本地/已 promote" --> PCBL
    end

    subgraph REPO["共享存储（冷路径兜底）"]
        OSS[("OSS repository")]
    end

    subgraph SCHED["Scheduler"]
        LOCATOR["memfile locator<br/>(新增：digest → 候选源节点列表)"]
        HINT["artifact→node hints<br/>(现有 P2P 路由复用)"]
    end

    UFFDB -- "① 查询 memfile digest 位置" --> LOCATOR
    LOCATOR -- "② 返回候选节点 (A 优先)" --> UFFDB
    UFFDB <== "③ urma/RDMA 远端读<br/>(按需 4K/大页粒度, 不落地)" ==> URMAA
    UFFDB -- "④ 源不可用 → 回退" --> OSS
    PCBL -. "⑤ 热页 promote<br/>(重复缺页阈值触发)" .-> UFFDB

    subgraph RootfsPath["rootfs 层（对比：走既有 P2P）"]
        RP2P["P2P client (现有)"]
        RP2P -- "层 digest 路由" --> HINT
        RP2P <== "对等拉取" ==> NodeA
        RP2P -- "miss → OSS" --> OSS
    end
```

---

## 3. 关键流程时序：跨节点 memfile resume

```mermaid
sequenceDiagram
    autonumber
    participant S as Scheduler
    participant B as 节点B uffd handler
    participant A as 节点A urma endpoint
    participant O as OSS

    Note over B: 沙箱 resume，FC 启动并注册 uffd 区间
    B->>S: locate(memfile_digest)
    S-->>B: [节点A(热), 节点C(温), OSS(冷)]
    B->>A: urma connect + 注册读域
    A-->>B: OK（A 侧 page cache 保持驻留）

    loop 缺页驱动（demand paging）
        B->>A: READ(gpa_offset, len)
        A-->>B: 页数据（远端内存/页缓存直读）
        B->>B: UFFDIO_COPY 进 guest RAM
    end

    Note over B: 反复缺页的页（热页）
    B->>B: promote：写入本地 memfile 缓存<br/>（复用 full_file_cache 机制）
    B->>B: 后续命中走本地 page cache

    alt 源节点故障 / 静默丢读（urma 冷启动窗口）
        B->>A: READ 超时/失败（重试后）
        B->>O: 回退 OSS 按需读该区间
        Note over B: 已 promote 的热页不受影响
    end

    Note over B: 沙箱 snapshot / teardown
    B->>S: 注销本节点对 digest 的持有
```

---

## 3.5 创建流程时序图（跨节点 resume 全生命周期）

```mermaid
sequenceDiagram
    autonumber
    participant U as 用户/API
    participant OR as orchestrator<br/>(节点B)
    participant S as Scheduler
    participant SB as sandbox 任务<br/>(节点B)
    participant FC as Firecracker<br/>(节点B)
    participant UH as uffd handler<br/>(节点B)
    participant A as urma export<br/>(节点A)

    U->>OR: POST /sandboxes (from snapshot)
    OR->>S: 请求调度（现有 assignment 路径）
    S-->>OR: 分配节点 B

    rect rgb(230, 240, 255)
        Note over OR,SB: 本地资源准备（现有流程不变）
        OR->>SB: 启动沙箱任务
        SB->>SB: 解析 snapshot → memfile digest
        SB->>SB: 建本地稀疏 memfile 缓存文件<br/>(full_file_cache 占位)
    end

    rect rgb(255, 245, 230)
        Note over SB,A: 新增：读源协商
        SB->>S: locate(memfile_digest)
        S-->>SB: [A(热), C(温), OSS(冷)]
        SB->>A: urma connect + 注册读域<br/>(引用计数 +1)
        A-->>SB: OK
        SB->>UH: 安装 Source 链<br/>Remote(A) → Local(cache) → OssRange
    end

    rect rgb(230, 255, 230)
        Note over SB,FC: 启动（现有 load_snapshot_uffd 路径）
        SB->>FC: spawn / pool consume
        SB->>FC: load_snapshot_uffd (uffd UDS)
        FC->>UH: 缺页 → UFFDIO_COPY
        UH->>UH: Local 命中? → Remote 读(A) → OSS 兜底
        UH->>UH: 热页 promote → 本地缓存文件
    end

    FC-->>SB: envd ready
    SB->>S: 注册本节点持有 digest<br/>(promote 达阈值后, B 成为候选源)
    SB-->>OR: Running
    OR-->>U: 201 sandbox_id
```

**创建流程关键点：**
1. **Source 链安装先于 FC 启动**——uffd 缺页风暴到来前读源已就绪（含 urma 冷启动首读重试，见 §5 风险）
2. **B 注册为源的条件**：promote 落地量达阈值——纯远端直读的节点不应注册为源（自己没有本地数据可服务）
3. 调度决策可以增强（现有 assignment 优先选已有 digest 持有的节点），但**不作为正确性依赖**——任何节点都能 resume，只是读源远近不同

## 3.6 删除流程时序图（引用归零 → 资源回收）

```mermaid
sequenceDiagram
    autonumber
    participant U as 用户/API
    participant OR as orchestrator<br/>(节点B)
    participant SB as sandbox 任务<br/>(节点B)
    participant FC as Firecracker<br/>(节点B)
    participant A as urma export<br/>(节点A)
    participant S as Scheduler
    participant KC as 内核 page cache<br/>(节点B)

    U->>OR: DELETE /sandboxes/{id}
    OR->>SB: teardown

    rect rgb(255, 235, 235)
        Note over SB,FC: 沙箱级清理（现有 teardown 路径）
        SB->>SB: cancel pageout (vPause 语义)
        SB->>FC: 停 vCPU / kill FC 进程
        FC-->>SB: 进程退出
        SB->>SB: 关闭 urma import 会话
        SB->>A: 读域注销 (引用计数 -1)
    end

    rect rgb(245, 240, 255)
        Note over SB,KC: 新增：digest 级引用回收
        SB->>SB: 本节点还有其他沙箱<br/>引用同一 digest?
        alt 有（同 memfile 多沙箱）
            Note over SB: 本地缓存保留<br/>仅减本地 refcount
        else 无（最后一个引用者）
            SB->>SB: full_file_cache LRU 逐出本地缓存
            SB->>KC: fadvise DONTNEED<br/>(回收 promote 落地的页, 补§上轮缺口)
            SB->>S: 注销 B 对 digest 的持有
        end
    end

    rect rgb(240, 250, 240)
        Note over A: 源节点侧异步收敛
        A->>A: 引用计数归零且本节点<br/>无本地持有者?
        alt 是
            A->>A: urma 区间 unexport<br/>(memfile page cache 保留,<br/>等 LRU/内存压力自然回收)
        else 否（A 自己的沙箱还在用）
            Note over A: export 保持,<br/>仅减远端读域计数
        end
    end

    SB-->>OR: teardown 完成
    OR-->>U: 200
    Note over S: OSS artifact 不动——<br/>durable 记录由 repository 生命周期管理<br/>(与沙箱删除解耦, 现有语义)
```

**删除流程关键点：**
1. **两级引用计数**：沙箱级（节点内同 digest 多沙箱）→ digest 级（跨节点 remote 读域 + 本地持有），归零才触发真回收
2. **删除的是"驻留"不是"数据"**：OSS durable 记录不随沙箱删除（现有 repository 语义），回收的只是内存驻留（page cache + urma 区间）和本地缓存文件
3. **源节点 export 卸载是异步 best-effort**：引用计数归零即可 unexport，无需与 B 的 teardown 同步完成——挂起的 export 只浪费注册内存，不影响正确性
4. **fadvise DONTNEED 挂在最后一个引用者 teardown**：正好补上"page cache 只进不出"的已知缺口（§1.5 前的分析），且天然满足"还有活跃消费者就不丢"的前提

---

## 4. 组件改动清单（开发者视角）

```mermaid
flowchart LR
    subgraph Existing["现有组件（复用）"]
        UFFDC["storage/uffd-core<br/>handler.rs"]
        P2PA["src/overlaybd/p2p<br/>artifact.rs (层身份)"]
        SCH["services/scheduler<br/>artifact hints"]
        OSSB["src/repository<br/>OSS backend"]
    end
    subgraph New["新增/修改"]
        UEXT["uffd-core: 远端后端分支<br/>(Source trait 抽象化)"]
        URMA["urma export/import<br/>内存区域服务端"]
        LOC["Scheduler: memfile locator<br/>(digest→节点, 心跳维护)"]
        PROM["热页 promote 策略<br/>(重复缺页计数→本地落地)"]
    end
    UFFDC --> UEXT
    UEXT --> URMA
    P2PA -. "层身份/路由模式参考" .-> LOC
    SCH --> LOC
    UEXT --> PROM
    UEXT -. "冷路径兜底" .-> OSSB
```

| 组件 | 改动 | 说明 |
| --- | --- | --- |
| `storage/uffd-core/handler.rs` | 中等 | 读源抽象成 trait（LocalMemfile / RemoteUrma / OssRange）；现有本地路径变 trait impl |
| urma 传输层 | 中-大 | export 侧：memfile 区间注册成远端可读；import 侧：READ 重试（冷启动静默丢弃窗口，hints 路径需验证） |
| `services/scheduler` | 小 | memfile digest → 持有节点列表；心跳上报本节点驻留的 memory artifact |
| 热页 promote | 小 | 复用 full_file_cache 的块缓存思想：重复缺页计数超阈值 → 本地写回 → 转本地命中 |
| pageout.rs / vPause | **零** | 共享发生在 uffd 读源层，与 vPause（FC 进程匿名页）正交 |
| 内核 | **零** | 全用户态（urma 用户态 RDMA 语义） |

---

## 5. 设计要点与风险

1. **rootfs 与 memfile 走不同路径的原因**：rootfs 层小而不可变、访问随机 → P2P 拉全量到本地缓存是终态；memfile 大（GB 级）、resume 只触碰工作集（vPause 画像已知约 30-60%）→ 按需远端读避免无谓落地。
2. **热页 promote 是必配项**：纯远端直读会让每个热页永远付网络延迟；promote 后稳态等价于今天的本地缓存，远端只服务冷页尾部。
3. **故障回退链**：urma 源故障 → OSS 区间读兜底（memfile artifact 本来就在 repository）；已 promote 页不受影响。风险半径 = 冷页 resume 变慢，不是正确性。
4. **已知风险（来自项目经验）**：
   - urma import 后首个 READ 可能静默丢弃（此前仅目录查询路径有重试掩盖，hints 直连路径未验证）→ 时序图 alt 分支的读重试必须实现；
   - SegmentRegistry 256 段 LRU 只 unregister 不清 scheduler 记录 → locator 的持有关系必须与本地逐出同步，否则跨节点指向幽灵源。
5. **与 scheduler 语义对齐**：locator 只做"查询路由"（类似 Redis 只读副本的 existing-sandbox 查询），不建立绑定——与"生命周期事件是 best-effort"的现有约束一致。

---

## 6. 验证方案

```text
Phase 0  单节点：Source trait 重构 + 本地 impl 行为不变（回归 uffd E2E）
Phase 1  双节点 urma 直读：工作集重放 profile，对比
         (a) OSS 全量拉取 resume   (b) urma 按需直读
         KPI: resume-to-ready、稳态缺页 p99、总网络流量（应 ≪ memfile 大小）
Phase 2  promote 阈值调优 + 故障注入（kill 源节点 → 验证 OSS 回退、已 promote 页无损）
Phase 3  接入 scheduler locator，多节点规模验证 hints 命中率
```
