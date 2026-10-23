# LSBT 缓存替换策略的形式化模型

> **论文附录稿**：本文以定义、编号公式与引理的形式，完整刻画仓库中 LSBT 系列实现的替换语义；每条公式均标注为**重点公式**（$\mathbf{KF}$，论文正文应单独列出并引用）或**普通定义**（$\mathbf{Def}$，仅用于形式化辅助），并在附录 A 给出索引。

## 摘要

LSBT 是一族面向**分层、带版本（versioned）** 工作负载的缓存替换策略：对象按访问证据获得热值，热值随逻辑版本时钟几何衰减，并被量化为“剩余寿命桶”；常驻集合由一个到期时间轮维护，满容量时按剩余寿命升序驱逐，同时以一位续约信用与容量为 $8C$ 的幽灵目录保留“刚被复用”与“近期被逐出”的键的证据。本文把该实现整理为一个封闭的状态机模型：给出热值递推式、分桶量化映射、状态转移与驱逐判定规则，证明其驱逐顺序与容量不变量，并形式化两级（Macro/Micro）扩展的容量、溢出与边界携带语义。所有公式与 `LSBTSimple`、`PackedLSBT`、`LSBTElasticCore`、`LSBTdiscrete`、`LSBTconsecutive` 的实现逐条对应。

---

## 1 引言

缓存替换的核心问题是：在容量 $C$ 约束下，维护一个常驻集合 $\mathcal{R}$，使未来的命中率尽可能高。经典策略以访问频度或新近度近似未来复用概率；LSBT 面向的 workload 具有两个额外结构：

1. **层级性**：对象位于树/前缀结构中，深度 $d$ 提供冷启动先验；
2. **版本性**：请求关联逻辑版本 $v$，版本边界是复用证据衰减的自然时钟。

LSBT 的工程实现（`LSBTSimple`、`PackedLSBT`、`LSBTElasticCore` 及两个基线变体）包含若干看似启发式的部件：热值分桶、时间轮、续约信用、幽灵目录。本文的目的并非重述代码，而是证明这些部件可以纳入**一组封闭的递推公式与判定规则**，从而为论文提供可引用、可验证的数学模型。

**贡献。** （i）给出热值—分桶—转移—驱逐的统一形式化模型（§3–§4）；（ii）证明驱逐顺序、准入门槛与容量三条不变量（§5）；（iii）精确刻画离散与连续两个基线的语义差异（§6）；（iv）形式化两级扩展的弹性容量与边界携带（§7）；（v）建立公式到实现的逐条映射（§8 与附录 A）。

## 2 系统模型与记号

**工作负载模型。** 请求序列为 $(v_i,k_i,d_i)_{i\ge1}$，其中 $v_i$ 是非降的逻辑版本号，$k_i$ 为对象标识，$d_i\in[1,D]$ 为对象深度。设版本时钟

$$
t \;\leftarrow\; \text{请求流中已完成的版本边界数},\qquad t=1,2,\dots
$$

每跨越一个版本边界，所有实现执行一次 `FinishBlock()`/`Tick()`。全文记号见表 1。

**表 1：记号约定**

| 记号 | 含义 | 默认值 |
| --- | --- | --- |
| $C$ | 常驻容量 | 运行参数 |
| $D$ | 最大深度 | 20（runner） |
| $\rho$ | 历史保留率 | 0.95，clamp 至 $[0.92,0.98]$ |
| $B$ | 分桶数 | 式 (6) |
| $b_{\max}$ | 最大桶编号 $B-1$ | — |
| $g(d)$ | 单次访问增益 | 式 (1)–(3) |
| $H_k(t)$ | 键 $k$ 在版本 $t$ 的热值 | 式 (4) |
| $\beta(H)$ | 热值→桶量化映射 | 式 (8) |
| $\Phi(d,b)$ | 访问转移函数 | 式 (9) |
| $e_k$ | 到期版本 | — |
| $b_k(t)$ | 剩余寿命（当前桶） | 式 (11) |
| $r_k\in\{0,1\}$ | 续约信用位 | 0 |
| $\mathcal{R},\mathcal{G}$ | 常驻/幽灵集合 | — |
| $G$ | 幽灵容量 | 式 (15) |

---

## 3 形式化模型：热值、分桶与转移

本节给出 LSBT 的公共核心：热值模型（§3.1）、分桶量化（§3.2）、状态更新（§3.3）与时间轮（§3.4）。除特别说明外，本节公式对全部实现成立；连续变体 `LSBTconsecutive` 不使用分桶（§6.2），可视为 $B\to\infty$ 的极限。

### 3.1 热值模型

**定义 1（访问增益）。** 每次访问 $k$ 对热值施加增益 $g(d)=s_{\mathrm{gain}}\cdot w(d)$，其中 $s_{\mathrm{gain}}$ 为增益标度，$w(d)$ 为深度权重。实现取两种参数化：

$$
g(d) \;=\; s_{\mathrm{gain}}\cdot w(d),
\qquad (1)
$$

**普通定义**：$s_{\mathrm{gain}}=1$ 时，深度仅分三类（`LSBTSimple` / `PackedLSBT` / `LSBTElasticCore`，对应 `StructuralWeight` 与 `Scoring::Transition` 的深度类别映射）：

$$
w(d) \;=\; \rho^{\,\min(d-1,\,2)},
\qquad
w(1)=1,\ w(2)=\rho,\ w(d\ge3)=\rho^{2};
\qquad (2)
$$

**普通定义**：$s_{\mathrm{gain}}=51.2$（`kDefaultGainScale`）时，深度权重连续（`LSBTdiscrete` / `LSBTconsecutive` 的 `DepthWeight`）：

$$
w(d) \;=\; \rho^{\,\mathrm{stride}\cdot(d-1)},\qquad
\mathrm{stride}=\begin{cases}
\dfrac{1}{D-1}, & D>1,\\[2mm]
0, & D=1.
\end{cases}
\qquad (3)
$$

> **注 1**。`LSBTconsecutive::SetCoverageBranchingFactor` 可将权重替换为 $w(d)=b_{\mathrm{cov}}^{-(d-1)}$；论文默认不使用。

**定义 2（热值）。** 键 $k$ 的热值为其历史访问增益的几何衰减和：

$$
\boxed{
H_k(t) \;=\; \sum_{\substack{a:\ k_a=k\\ t_a\le t}} g(d_a)\cdot \rho^{\,t-t_a}
}
\qquad (4)
$$

> **🔑 重点公式 $\mathbf{KF\text{-}1}$（热值递推）。** 式 (4) 是全文第一个关键公式：它同时给出“每次访问叠加增益”与“按版本几何衰减”的闭式表达，是分桶量化（式 (8)）与驱逐排序（引理 1）的语义基础。实现上，代码以对数空间避免溢出：定义归一化热值
>
> $$
> \ell_k(t) \;=\; \log H_k(t) - t\log\rho,
> \qquad (5)
> $$
>
> **普通定义**。访问时执行 $\ell_k \leftarrow \log\!\big(e^{\ell_k}+g(d)\rho^{-t}\big)$（`LogAddExp`），与式 (4) 严格等价；驱逐堆仅需比较 $\ell_k$，因同一版本 $t$ 下公共项 $t\log\rho$ 不改变排序。

### 3.2 分桶量化

分桶实现的动机是：热值只用于比较，故可量化为“剩余寿命”桶 $b\in\{0,\dots,B-1\}$，使常驻键仅需维护一个到期版本 $e_k$。

**定义 3（分桶结构）。** 桶数、参考热值与代表热值分别为

$$
B \;=\; \max\!\Big(1,\ \mathrm{round}\!\big(32\log_2(C+1)\big)\Big),
\qquad (6)
$$

$$
H_{\mathrm{ref}} \;=\; \frac{s_{\mathrm{ref}}}{1-\rho},\qquad
H_{\min} \;=\; H_{\mathrm{ref}}\cdot \rho^{\,b_{\max}},\qquad
H_b \;=\; H_{\min}\cdot \rho^{-b}.
\qquad (7)
$$

**普通定义**。其中 $\mathrm{round}(\cdot)$ 即 `llround`；$s_{\mathrm{ref}}=1$（simple/packed/elastic），或 $s_{\mathrm{ref}}=51.2$（`LSBTdiscrete` 的 `reference_gain_scale`）；式 (6)–(7) 对应 `ComputeBucketCount` 与 `BuildTransitionTable` 中的 `representative_heat_`。连续变体无分桶，见 §6.2。

**定义 4（量化映射）。** 热值到桶的映射为

$$
\boxed{
\beta(H) \;=\; \mathrm{clamp}\!\left(
\mathrm{round}\!\left(
\frac{\log\big(\max(H,\varepsilon)/H_{\min}\big)}{\log(1/\rho)}
\right),\ 0,\ b_{\max}
\right),
\qquad \varepsilon=10^{-12}
}
\qquad (8)
$$

> **🔑 重点公式 $\mathbf{KF\text{-}2}$（量化映射）。** 式 (8) 是离散实现的热值编码规则：热值 $H$ 被编码为“相对最小热值的对数档位”，四舍五入并截断到 $[0,b_{\max}]$。它决定了一个访问增益能把键推高几个寿命档，直接决定驻留时长；连续变体取 $B\to\infty$ 后即回归式 (4) 的精确排序。

### 3.3 状态更新：转移、到期与剩余寿命

**定义 5（访问转移）。** 键 $k$ 在版本 $t$ 被访问时，从当前桶 $b_k(t)$ 转移到

$$
\boxed{
\Phi(d_{\mathrm{ev}}, b) \;=\; \beta\big(H_b + g(d_{\mathrm{ev}})\big)
}
\qquad (9)
$$

$$
b^+ \;=\; \Phi\big(d_{\mathrm{ev}},\ b_k(t)\big),\qquad
e_k \;\leftarrow\; t + b^+,
\qquad (10)
$$

> **🔑 重点公式 $\mathbf{KF\text{-}3}$（访问转移与到期更新）。** 式 (9)–(10) 是状态机的核心递推：访问把“当前代表热值 + 本次增益”重新量化，并把到期版本前移 $b^+$ 个版本。转移表在构造时预计算（`BuildTransitionTable`），运行时为 $O(1)$ 查表。

其中证据深度 $d_{\mathrm{ev}}$ 的取法：

- **简化核心**（`LSBTSimple::Access`、`PackedLSBT::MacroLSBT::Access`、`LSBTElasticCore::Access`）：
  $$
  d_{\mathrm{ev}} \;=\; \begin{cases} d, & \text{该键为全新观察（inserted）},\\ 1, & \text{否则（常驻命中或 ghost 命中）}. \end{cases}
  $$
  **普通定义**。其语义是：热值证据的“强度”由首次观察的深度决定，重复观察只施加最小深度的增量。
- **离散/连续变体**：恒取 $d_{\mathrm{ev}}=d$。**普通定义**。在逐版本去重后的 block trace 上，每个版本每个键至多出现一次，故两规则结果一致。

**定义 6（剩余寿命）。** 键 $k$ 在版本 $t$ 的当前桶（即剩余寿命）为

$$
b_k(t) \;=\;
\begin{cases}
e_k - t, & 0 < e_k - t \le b_{\max},\\
0, & \text{否则}.
\end{cases}
\qquad (11)
$$

**普通定义**（实现细节：代码以无符号减法统一处理“过期”与“从未见过”，即式 (11) 中 $e_k-t\le0$ 的情形）。$b_k(t)$ 虽为定义，但它是驱逐排序（引理 1）与准入门槛（引理 2）的直接输入，下文重点公式均以其为变量。

### 3.4 时间轮：按剩余寿命的确定性调度

时间轮是“按到期版本排序的 FIFO 调度器”，其行为可精确刻画（`LSBTTimingWheel.hpp`、`PackedLSBT.hpp` 的 `TimingWheel`）。

**定义 7（调度与推进）。** 轮盘含 $B$ 个桶与游标 $c\in\{0,\dots,B-1\}$：

$$
p \;=\; (c+m)\bmod B,
\qquad (12)
$$

**🔑 重点公式 $\mathbf{KF\text{-}4}$（调度槽）。** 剩余寿命为 $m$ 的键被放入桶 $p$ 的 FIFO 尾部（$m=0$ 时直接入“过期队列”）。每推进一个版本：$c\leftarrow(c+1)\bmod B$，桶 $c$ 的整条 FIFO 并入过期队列尾部。

$$
q \;=\; (p-c)\bmod B,
\qquad (13)
$$

**普通定义**（物理剩余）。候选为过期队列队首；若为空，则按 $c+1,c+2,\dots,B-1,0,\dots,c-1$ 顺序取第一个非空桶的队首。

> **引理 1（驱逐顺序）。** 时间轮 `Front()` 返回的候选，是当前所有常驻键中**剩余寿命最小者**；剩余寿命并列时按进入时间 FIFO。
>
> *证明。* 过期队列由各桶按到期版本升序并入，且桶内为 FIFO，故过期队列严格按到期版本升序、同版本按进入顺序排列。非过期桶的物理剩余 $q=(p-c)\bmod B$ 随 $p$ 的遍历顺序严格递增，而当前版本下到期版本 $e=t+q$，故桶序遍历等价于剩余寿命升序。过期队列（$e\le t$，剩余寿命为 0）恒先于非过期候选。证毕。∎

---

## 4 算法语义：标准核心

标准核心指 `LSBTSimple`、`PackedLSBT::MacroLSBT/MicroLSBT` 与 `LSBTElasticCore` 共同实现的访问—准入—驱逐状态机，是两级扩展（§7）与各基线（§6）的公共内核。

### 4.1 状态空间与幽灵目录

**定义 8（状态空间）。** 每个键维护三元状态

$$
s_k \in \{\text{absent},\ \text{ghost},\ \text{resident}\},
\qquad
r_k\in\{0,1\},
\qquad (14)
$$

**普通定义**。$r_k$ 为一位续约信用。常驻集合 $\mathcal{R}$ 由时间轮维护；幽灵集合 $\mathcal{G}$ 是容量

$$
G \;=\; \begin{cases}
\lceil 8C\rceil, & \text{simple/packed/elastic（封顶 } \mathrm{INT32\_MAX}),\\
\lfloor 8C\rfloor, & \text{discrete/consecutive},
\end{cases}
\qquad (15)
$$

的 FIFO（**普通定义**）。幽灵语义：miss 时把键插入 $\mathcal{G}$ 队尾；队首被挤出时彻底删除其元数据；ghost 命中仍计为 miss，但保留其 $e_k$ 并施加证据更新（式 (10)，**普通定义**）。

### 4.2 访问过程 `Access(k,d)`

**定义 9（访问）。** 给定 $(k,d)$，执行：

1. 若 $k\notin\mathcal{R}$：把 $k$ 插入幽灵 FIFO（已在 $\mathcal{G}$ 中则无操作）；
2. 按式 (9)–(10) 计算 $b^+$ 并更新 $e_k\leftarrow t+b^+$；
3. 若 $k\in\mathcal{R}$（命中）：置 $r_k\leftarrow 1$。

返回命中标志。**普通定义**（状态更新规则，直接对应 `Access`）。

### 4.3 准入与驱逐 `Admit(k)` / `EvictOne(b_{\mathrm{in}})`

设 `Access` 刚为 $k$ 计算出入桶寿命

$$
b_{\mathrm{in}} \;=\; b_k(t).
\qquad (16)
$$

**普通定义**。准入过程为：

1. 若 $k\in\mathcal{R}$：命中，返回 true；
2. 若容量上限为 0（Macro 变体以 `resident_limit` 为准，§7.3）：拒绝；
3. 若 $k\in\mathcal{G}$：先从幽灵 FIFO 取出（驱逐失败则恢复）；
4. 若 $|\mathcal{R}|\ge$ 容量上限：调用 `EvictOne(b_{\mathrm{in}})`；失败则恢复 ghost 并返回 false；
5. 成功：`InsertResident`——$r_k\leftarrow 0$，调度到桶 $b_k(t)$。

**普通定义**（流程）。核心判定在驱逐过程：

> **🔑 重点规则 $\mathbf{KF\text{-}5}$（驱逐判定）。** 在容量已满时，`EvictOne(b_{\mathrm{in}})` 循环执行：设 $x$ 为时间轮候选（引理 1 的顺序），记 $a=b_x(t)$ 为实际剩余，$q$ 为物理剩余：
>
> (R1) **懒救援**：若 $a>q$（$x$ 的到期版本在调度后被延长），弹出并以 $m=a$ 重新调度，继续；
> (R2) **准入门槛**：若 $b_{\mathrm{in}}\le a$，拒绝准入并清空 $r_x$，返回失败；
> (R3) **续约信用**：若 $r_x=1$，则弹出 $x$，令 $e_x\leftarrow t+b_{\mathrm{in}}$，以 $m=b_{\mathrm{in}}$ 重新调度，清空 $r_x$，继续；
> (R4) **驱逐**：否则弹出 $x$，置 $s_x\leftarrow\text{ghost}$ 并插入幽灵 FIFO，返回成功。

$KF\text{-}5$ 蕴含两条可独立陈述的语义：**（i）准入门槛**——新键只有剩余寿命严格大于当前最冷常驻键时才可准入（$b_{\mathrm{in}}>a$，R2）；**（ii）一位续约信用**——被命中过的键在成为最冷候选时有一次以入桶寿命续约的机会（R3），而不是被直接驱逐。二者共同构成“证据保护”机制。

> **注 2（复杂度）。** 式 (9) 的转移为 $O(1)$ 查表；时间轮调度、推进与队首访问均为 $O(1)$。`EvictOne` 的循环在续约/救援密集时可超过 $O(1)$，但每次续约或救援都消耗一个信用位或把键重新调度到更晚的槽位，平摊代价为 $O(1)$。

---

## 5 关键性质

**引理 2（准入门槛的等价刻画）。** 在标准核心中，满容量时 `Admit(k)` 成功当且仅当存在常驻键 $x$ 满足

$$
b_x(t) < b_{\mathrm{in}} \quad\text{且}\quad r_x = 0;
\qquad (17)
$$

且被驱逐的是满足式 (17) 的键中剩余寿命最小者（并列时按时间轮 FIFO 序）。

> *证明。* 由引理 1，时间轮候选按剩余寿命升序到达。**必要性**：若拒绝发生在某候选 $x$（R2，$b_x(t)\ge b_{\mathrm{in}}$），则因候选按 $b$ 升序，其后的所有键均满足 $b\ge b_{\mathrm{in}}$，故不存在 $b_x(t)<b_{\mathrm{in}}$ 的键，式 (17) 不成立。**充分性**：若式 (17) 成立，取满足条件者中 $b$ 最小者 $x^{*}$；扫描过程中，$x^{*}$ 之前的候选要么因实际剩余被救援（R1）、要么带信用被续约（R3）、要么本身满足 R4 条件而被驱逐，均不会触发 R2（其 $b<b_{\mathrm{in}}$）；$x^{*}$ 到达时 $r_{x^{*}}=0$ 且 $b_{x^{*}}<b_{\mathrm{in}}$，必被 R4 驱逐。若满足 $b<b_{\mathrm{in}}$ 的键全部带信用，则它们依次被续约后，循环必然到达首个 $b\ge b_{\mathrm{in}}$ 的候选并触发 R2 拒绝，与式 (17) 成立的前提矛盾。被驱逐者即扫描顺序下最先满足式 (17) 的键。证毕。∎

> **🔑 重点公式 $\mathbf{KF\text{-}6}$（准入门槛）。** 式 (17) 是 $\mathbf{KF\text{-}5}$ 规则 R2–R4 的闭式刻画，论文正文可用它替代逐条伪代码：新键仅在“比当前最冷无信用键更热”时准入。

**引理 3（容量不变量）。** 标准核心始终满足 $|\mathcal{R}|\le C$。

> *证明。* `InsertResident` 仅在 $|\mathcal{R}|<$ 容量上限时执行：若已满，先调用 `EvictOne`，仅当返回成功（$|\mathcal{R}|$ 减一）后插入；`EvictOne` 拒绝路径不改动 $\mathcal{R}$。由归纳立即得证。证毕。∎

**命题 1（连续模型等价性）。** `LSBTconsecutive` 的驱逐顺序等价于“按剩余寿命升序”：其最小堆按热值 $H_k(t)$ 排序，而在连续模型中热值与剩余寿命经式 (8) 的逆映射单调对应。

> *证明。* 堆序键为 $\ell_k(t)$；同一版本下 $\ell_k(t)=\log H_k(t)-t\log\rho$，公共项 $t\log\rho$ 不改变相对顺序，故堆序等价于 $H_k(t)$ 升序。由式 (7)–(8)，$H$ 与寿命 $b=\log(H/H_{\min})/\log(1/\rho)$ 单调递增，故与剩余寿命升序一致。证毕。∎

**注 3（离散近似的误差源）。** 离散变体的驱逐基于“调度时刻的寿命桶”而非“当前剩余寿命”（§6.1），因此命题 1 的等价性只对连续变体与时间轮核心严格成立；分桶与静态桶队列引入量化与陈旧调度两项确定性误差，但均不改变状态机的确定性。

---

## 6 基线变体

### 6.1 `LSBTdiscrete`：静态桶队列

`LSBTdiscrete` 与标准核心共享 §3 的热值、分桶与转移，但驱逐器有三处差异：

1. **无续约信用**：状态空间式 (14) 中不包含 $r_k$；
2. **无准入门槛**：`EvictOne()` 不接受入桶参数，式 (16) 的 $b_{\mathrm{in}}$ 计算后未参与判定，即引理 2 不成立；
3. **静态桶队列替代旋转时间轮**：`FinishBlock()` 只推进版本并清除保护，不推进轮盘游标；键被调度到“调度时刻的剩余寿命桶”，之后除非被救援，一直留在该桶。驱逐从最小非空桶编号向上扫描（`current_cold_bucket_`），桶内 FIFO；候选实际剩余 $a>b$ 时救援到桶 $a$（`rescue_limit` 默认无限）。

**普通定义**（差异规则）。此外，离散变体提供每版本保护纪元：候选被保护时旋转到桶 $\max(a,b)$ 队尾，旋转达到 `protect_scan_budget`（默认 16）则拒绝准入；关闭旋转（`rotate_protected_candidates_=false`）时直接拒绝。**普通定义**。其可选动态保护/评分钩子为实验接口，不属于核心状态机。

> **注 4。** `LSBTdiscrete.hpp` 构造参数中的 `first_touch_per_block_`、`repeat_hit_gain_scale_`、`reward_window_capacity_ratio_` 仅被赋值、从未参与逻辑，属历史遗留，论文不应引用。

### 6.2 `LSBTconsecutive`：精确热值与租约

连续变体不使用式 (6)–(11) 的分桶结构，热值直接采用式 (4)（$\mathbf{KF\text{-}1}$），驱逐由最小堆按 $H_k(t)$ 升序完成（命题 1）；失效堆项（代数不匹配）跳过；保护键跳过，累计达到 `protect_scan_budget`（16）则拒绝准入。

连续变体还可选开启**续约租约**（`enable_renewal_lease_`，默认关闭），为跨版本复用证据附加硬性周转上限：

$$
\boxed{
\mathrm{lease}(k) \;=\; t + \min\!\big(L_{\max},\ L_0 + p_k\,L_{\mathrm{step}}\big)
}
\qquad (18)
$$

> **🔑 重点公式 $\mathbf{KF\text{-}7}$（租约长度）。** 其中 $p_k$ 为键 $k$ 的不同版本命中次数（每次新版本命中加 1，封顶 3）；默认 $L_0=2$，$L_{\mathrm{step}}=2$，$L_{\max}=8$。租约过期的键无论热值高低均进入驱逐候选（除非被保护），对应 `EvictExpiredLease` 优先于最小堆执行。该公式只在启用租约的变体实验中相关。

---

## 7 两级扩展：Macro 与 Micro

当前主实现（`TwoTimescaleCacheSparse` + `PackedLSBT`）将 §4 的标准核心实例化为两个**热值互不共享**的副本：**Macro**（跨版本持久）与 **Micro**（版本内复用）。

### 7.1 Macro 的首次触碰语义

每个版本 $v$ 内，键 $k$ 仅在其**首次出现**时调用 `Macro::Access`；同版本内的后续命中只返回结果，不更新 Macro 证据（`first_touch` 分支）。**普通定义**（与式 (10) 组合，即“版本内重复访问不产生跨版本热值”）。

### 7.2 Micro 容量

$$
\boxed{
C_M(v) \;=\; \max\!\Big(1,\ \min\!\big(C,\ \mathrm{round}(\tfrac{2}{3}C),\ U_{v-1}>0 \;?\; \mathrm{round}(\tfrac{1}{2}U_{v-1}) : \mathrm{round}(\tfrac{2}{3}C)\big)\Big)
}
\qquad (19)
$$

> **🔑 重点公式 $\mathbf{KF\text{-}8}$（Micro 容量）。** $U_{v-1}$ 为上一版本的**去重足迹**（该版本内出现过的不同键数）；首版本使用结构上限 $\mathrm{round}(2C/3)$。式 (19) 使容量分配自适应工作负载的版本内复用压力：去重足迹越大，Micro 可占用的上限越大（至多饱和到 $\mathrm{round}(2C/3)$）。

### 7.3 弹性容量与溢出

$$
\boxed{
|\mathcal{R}_{\mathrm{Macro}}| + |\mathcal{R}_{\mathrm{Micro}}| \;\le\; C,
\qquad
\mathrm{MacroLimit} \;=\; C - |\mathcal{R}_{\mathrm{Micro}}|
}
\qquad (20)
$$

> **🔑 重点公式 $\mathbf{KF\text{-}9}$（弹性容量不变量）。** 式 (20) 保证常驻总量严格不超过 $C$（`SyncMacroLimitAndSpill`）：Micro 增长时 Macro 以 `SetResidentLimit` 收缩并强制驱逐。

**定义 10（溢出与边界携带）。** 两级扩展的跨层规则：

> **🔑 重点规则 $\mathbf{KF\text{-}10}$（溢出与携带）。**
>
> (S1) **Macro→Micro 溢出**：被 Macro 强制驱逐的键，仅当其在本版本内已发生真实复用（`IsReused`）时，才被 `Micro::Admit` 接收；
> (S2) **边界携带**：版本结束时，Micro 中“本版本真实复用过”且仍驻留的键，经 `Macro::Admit` 携带至 Macro——**不调用 `Macro::Access`**，即不伪造跨版本热值；
> (S3) **重置**：随后 `Macro::Tick()` 推进版本时钟，Micro 整体重置（容量按式 (19) 重算，时钟归 1；Micro 无幽灵，无需清理）。

**普通定义**（流程规则）。$\mathbf{KF\text{-}10}$ 的两条规则共同保证“版本内复用证据可以被保住，但不转化为跨版本热值”。

---

## 8 其他机制与实现映射

### 8.1 分数周转时钟（可选）

当工作负载无语义版本、需用控制版本时，`LSBTSimple` 提供 `EnableFractionalTurnoverClock` / `FinishFractionalTurnoverUnit`：

$$
\text{credit} \mathrel{+}= \frac{\Phi(d,0)}{C}\ \text{（每次 miss）},\qquad
\text{ticks} = \lfloor\text{credit}\rfloor,\quad
\text{credit} \mathrel{-}= \text{ticks},
\qquad (21)
$$

**普通定义**。每单位推进 `ticks` 个版本。这是“缓存周转一次约等于累计 $C$ 个单位寿命”的工程近似；两级封装的 Twitter replay 使用固定控制版本 $L=2C$（`code/two_timescale/README.md` §1.4），与本机制为两种独立实现。

### 8.2 校准桶数（可选）

`LSBTdiscrete` 在 `one_touch_lifetime=L>0` 时改用校准桶数，使单次触碰 $L$ 个版本后热值仍高于最小热值：

$$
\text{heat\_ratio} \;=\; \frac{1+(a-1)(1-\rho)}{(1-\rho)\,\rho^{L}},\qquad
B \;=\; \Big\lceil \frac{\log(\text{heat\_ratio})}{\log(1/\rho)} \Big\rceil + 1,\qquad
H_{\min} \;=\; s_{\mathrm{gain}}\,\rho^{L},
\qquad (22)
$$

**普通定义**。$a$ 为 `burst_accesses`；默认不启用（$L=0$）。

### 8.3 公式到实现的映射

| 公式 | 内容 | 代码位置 |
| --- | --- | --- |
| (1)–(3) | 增益与深度权重 | `StructuralWeight`、`Scoring::Transition`、`DepthWeight` |
| (4)–(5) | 热值递推（对数空间） | `ObserveAccess`、`LogAddExp`、`DebugCurrentHeat` |
| (6)–(7) | 桶结构与参考热值 | `ComputeBucketCount`、`BuildTransitionTable` |
| (8) | 量化映射 | `HeatToBucket` |
| (9)–(10) | 转移与到期更新 | `BuildTransitionTable`、`Access` |
| (11) | 剩余寿命 | `CurrentBucket` |
| (12)–(13) | 时间轮调度与物理剩余 | `TimingWheel::Schedule`、`Front`、`PhysicalRemaining` |
| (14)–(15) | 状态空间与幽灵容量 | `LSBTState.hpp`、`ComputeGhostCapacity`、`GhostFIFO` |
| (16)–(17) | 入桶寿命与准入门槛 | `Admit`、`EvictOne` |
| (18) | 租约长度 | `LeaseDuration`、`RenewLease`、`EvictExpiredLease` |
| (19)–(20) | Micro 容量与弹性不变量 | `MicroCapacity`、`SyncMacroLimitAndSpill` |
| 规则 R1–R4 | 驱逐判定 | `EvictOne`（simple/packed/elastic） |
| 规则 S1–S3 | 溢出与携带 | `HandleMacroVictim`、`SelectiveBoundaryCarry`、`EndEpoch` |
| (21)–(22) | 分数时钟与校准桶数 | `ObserveFractionalTurnoverMiss`、`ComputeCalibratedBucketCount` |

### 8.4 默认参数

| 参数 | 默认值 |
| --- | --- |
| $\rho$ | 0.95（clamp $[0.92,0.98]$） |
| $s_{\mathrm{gain}}$ | 1（simple/packed/elastic）；51.2（discrete/consecutive） |
| $s_{\mathrm{ref}}$ | 1（simple/packed/elastic）；51.2（discrete） |
| $G$ | $8C$ |
| $d_{\mathrm{ev}}$ | 新键 $=d$，其余 $=1$（simple/packed）；恒 $=d$（discrete/consecutive） |
| 保护扫描预算 | 16（discrete/consecutive） |
| 租约 $L_0/L_{\mathrm{step}}/L_{\max}$ | 2 / 2 / 8（consecutive，默认关闭） |

---

## 9 结论与讨论

LSBT 的实现逻辑可以完整总结为一组封闭的递推公式与判定规则：热值由式 (4) 递推、由式 (8) 量化、经式 (9)–(10) 转移为到期版本，常驻键按引理 1 的顺序驱逐，准入由式 (17) 判定，两级扩展由式 (19)–(20) 与规则 S1–S3 刻画。

论文写作建议：

1. **正文核心公式**为 $\mathbf{KF\text{-}1}$–$\mathbf{KF\text{-}6}$（热值、量化、转移、时间轮、驱逐规则、准入门槛），其余作为定义或附录材料；
2. 若采用两级设计，另列 $\mathbf{KF\text{-}8}$–$\mathbf{KF\text{-}10}$；
3. 涉及租约实验时引用 $\mathbf{KF\text{-}7}$；
4. 引理 1–3 可直接作为性质陈述，证明可在附录给出。

确定性但属于工程细节的部分（离散变体的静态桶队列、保护旋转的预算行为、ghost 命中的证据合并、数值常量 $\varepsilon$）不改变模型的形式化完备性，但应在复现说明中注明。

---

## 附录 A：公式索引

**重点公式（$\mathbf{KF}$）**

| 编号 | 公式 | 一句话含义 | 出处 |
| --- | --- | --- | --- |
| $\mathbf{KF\text{-}1}$ | (4) | 热值为访问增益的几何衰减和 | §3.1 |
| $\mathbf{KF\text{-}2}$ | (8) | 热值到剩余寿命桶的对数量化 | §3.2 |
| $\mathbf{KF\text{-}3}$ | (9)–(10) | 访问转移与到期版本更新 | §3.3 |
| $\mathbf{KF\text{-}4}$ | (12) | 时间轮调度槽 | §3.4 |
| $\mathbf{KF\text{-}5}$ | 规则 R1–R4 | 驱逐判定（救援/门槛/续约/驱逐） | §4.3 |
| $\mathbf{KF\text{-}6}$ | (17) | 准入门槛：$b_x(t)<b_{\mathrm{in}}\wedge r_x=0$ | §5 |
| $\mathbf{KF\text{-}7}$ | (18) | 租约长度（可选变体） | §6.2 |
| $\mathbf{KF\text{-}8}$ | (19) | Micro 容量 | §7.2 |
| $\mathbf{KF\text{-}9}$ | (20) | 弹性容量不变量 | §7.3 |
| $\mathbf{KF\text{-}10}$ | 规则 S1–S3 | Macro→Micro 溢出与边界携带 | §7.3 |

**普通定义（$\mathbf{Def}$）**

| 编号 | 公式 | 内容 |
| --- | --- | --- |
| Def-1 | (1)–(3) | 访问增益与深度权重（两种参数化） |
| Def-2 | (5) | 对数归一化热值（数值技巧） |
| Def-3 | (6)–(7) | 桶数、参考热值、代表热值 |
| Def-4 | (11) | 剩余寿命（当前桶） |
| Def-5 | (13) | 时间轮物理剩余 |
| Def-6 | (14)–(15) | 状态空间与幽灵容量 |
| Def-7 | (16) | 入桶寿命 |
| Def-8 | (21) | 分数周转时钟 |
| Def-9 | (22) | 校准桶数 |

**引理 / 命题**

| 编号 | 内容 | 证明 |
| --- | --- | --- |
| 引理 1 | 时间轮候选为剩余寿命最小者，同寿命 FIFO | §3.4 |
| 引理 2 | 准入门槛等价刻画（式 (17)） | §5 |
| 引理 3 | 容量不变量 $|\mathcal{R}|\le C$ | §5 |
| 命题 1 | 连续变体驱逐顺序等价于剩余寿命升序 | §5 |
