# Wolvicmod

Wolvicmod 是一个**结构化 CModel 框架**：面向高性能 CPU、NPU 这类内部紧耦合微结构（密集零延迟互馈：流水线握手、旁路前递、互斥仲裁）的电子系统级（ESL）行为级建模与仿真。

---

## 1. 简介

### 建模粒度的选择

Wolvicmod 取现有方案的中间粒度。三种框架在结构粒度、行为粒度及其后果上的对比：

| 框架 | 结构粒度 | 行为粒度 | 依赖关系与静态检查 | 仿真开销 |
|---|---|---|---|---|
| **RTL** | 每根线、每个比特 | 位级展开（排序网络、多周期状态机） | 结构信息完整但过细：位级精度超出微结构验证的需要 | 高：一次总线传输对应数十个位级翻转事件的调度 |
| **SystemC** | 丢失：敏感表只声明进程唤醒条件，不声明读写对象，执行次序由事件内核运行时动态决定 | 宿主语言级别，自由 | 不可静态检查：零延迟互馈靠人工安排 delta cycle，错误暴露为运行时行为偏差而非可定位的报错 | 低：一次传输是一次函数调用内的对象传递 |
| **Wolvicmod** | 依赖关系机器可读所需的最小粒度：状态、信号、每条计算的读集与目标显式声明 | 宿主语言级别：每条计算是黑盒 lambda，体内任意 C++ | 全部显式声明：多驱动、悬空、跨级连线在仿真前暴露，错误必附层次路径 | 低：同 SystemC，传输即对象传递 |

一句话概括 Wolvicmod 的位置——**结构显式声明，行为黑盒保留**：结构粒度不多（不到位级）、不少（不丢依赖），行为粒度停留在宿主语言表达力最强的级别。

### 主要特性

- **五种结构实体**：`Module`、`In<T>` / `Out<T>`、`Wire<T>`、`Reg<T>`、`Mem<T, R>`；类型参数 `T` 是任意 C++ 类型（STL 容器、自定义类均可），计算直接在原生类型上求值
- **两种计算动作**：Assign（组合逻辑，Wire 静态单赋值）与 Update（时序逻辑，边沿触发 + 电平守卫 + 注册序优先级）
- **elaboration 静态检查**：多驱动、悬空、未命名实体、跨级连线，全部在仿真前暴露，错误必附层次路径
- **仿真语义对齐 Verilator**：`set()` → `eval()` → `get()`；eval 幂等；round 内两阶段（组合求值 + NBA 提交）；级联时钟同一 eval 内逐级推进；组合环 SCC 迭代求稳态，振荡有上限保护
- **调试与观察**：FST 波形（GTKWave 可读）、round 级事件 trace、读集对账（偷读检测）、Update 互斥断言
- **header-only C++20**：零强制依赖（FST 波形经可选的 vendored libfst，MIT）

---

## 2. 快速上手案例

### 环境要求

- C++20 编译器（GCC 13+ / Clang 17+ 验证过）
- CMake ≥ 3.20
- zlib（仅 FST 波形需要）

### 集成

header-only，CMake `add_subdirectory` 即可：

```cmake
add_subdirectory(path/to/wolvicmod)
target_link_libraries(your_target PRIVATE wolvicmod)
```

### 第一个模型：带复位与使能的计数器

```cpp
#include <cstdio>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

struct Counter : Module {
    IN(bool, clk);
    IN(bool, rst_n);
    IN(bool, en);
    OUT(uint32_t, dout);
    REG(uint32_t, cnt);

    Counter() {
        cnt.update().on(negedge(rst_n)) = 0;                 // 复位：先注册，优先级高
        cnt.update().on(posedge(clk)).en(en).reads(cnt)      // 计数：上升沿且 en 有效
            = [](auto src) {
                auto [c] = src;
                return c + 1;
            };
        dout = cnt;
    }
};

int main() {
    Counter top;              // 构造完成：全部结构已注册
    top.elaborate();          // 展平、结构检查、建图

    top.rst_n.set(1); top.eval();
    top.rst_n.set(0); top.eval();   // 下降沿：复位生效，cnt ← 0
    top.rst_n.set(1); top.eval();

    top.en.set(1);
    for (int cycle = 0; cycle < 10; ++cycle) {
        top.clk.set(0); top.eval();
        top.clk.set(1); top.eval(); // 上升沿：cnt 自增
        printf("%d: %u\n", cycle, top.dout.get());
    }
}
```

输出为复位后每拍自增：`0: 1`、`1: 2`、…、`9: 10`。

### 更多案例

完整可运行的案例程序在配套的 [wolvicmod-playground](https://github.com/Wolf-Tungsten/wolvicmod-playground) 仓库的 [`example/`](https://github.com/Wolf-Tungsten/wolvicmod-playground/tree/main/example) 目录下（playground 以 git submodule 方式引用本仓库），覆盖从组合逻辑到时序、层次、存储器的典型场景，均可编译运行并自校验：

| 程序 | 覆盖点 |
|---|---|
| `adder.cpp` | 组合逻辑：Assign 完整形式与快速路径 |
| `counter.cpp` | 时序逻辑：Update、边沿、守卫、优先级；根模块驱动流程 |
| `sort_pipeline.cpp` | 层次与参数化：模板子模块、SUB、连线三模式、STL 容器状态 |
| `data_mem.cpp` | 存储器：Mem 写口 `.addr`、行级提交、组合读口 |

---

## 3. API 介绍

全部符号位于命名空间 `wolvicmod`，伞头文件为 `<wolvicmod/wolvicmod.h>`。诊断错误统一抛出 `wolvicmod::Error`（继承 `std::runtime_error`），消息必附层次路径。

### 3.1 模块与结构声明

模块是继承 `Module` 的普通类（可为模板类），建模全部在构造函数中完成。结构成员由创建方法声明，创建时命名（名字即诊断与波形中的层次路径）；六个**声明宏**是对应创建方法的简写形式，二者一一对应：

| 创建方法 | 声明宏 | 声明的实体 |
|---|---|---|
| `In<T>& createIn<T>(name)` | `IN(T, name)` | 输入端口 |
| `Out<T>& createOut<T>(name)` | `OUT(T, name)` | 输出端口 |
| `Wire<T>& createWire<T>(name)` | `WIRE(T, name)` | 组合信号 |
| `Reg<T>& createReg<T>(name)` | `REG(T, name)` | 寄存器 |
| `Mem<T, R>& createMem<T, R>(name)` | `MEM(T, R, name)` | 存储器（R 行 T） |
| `T& createChildModule<T>(name)` | `SUB(T, name)` | 子模块 |

类型参数含逗号时（如 `std::array<T, N>`），先取别名再进宏：

```cpp
struct Adder : Module {
    IN(uint32_t, a);          // = In<uint32_t>& a = createIn<uint32_t>("a");
    IN(uint32_t, b);
    OUT(uint32_t, sum);
    Adder() { sum = a + b; }
};
```

规则：模块不可拷贝；重名创建即报错；未经创建方法的实体注册动作时报错。

### 3.2 Assign：组合逻辑

完整形式——显式读集 + lambda（lambda 收到 `std::tuple<const Ts&...>`，顺序同 `reads` 实参）：

```cpp
sum.assign().reads(a, b) = [](auto src) {
    auto [x, y] = src;
    return x + y;
};
```

编译期校验：结构化绑定数与读集大小不匹配编译失败；lambda 不能以该 tuple 调用（`is_invocable` 校验）编译失败；读集以 `const&` 传入，lambda 内写读集编译失败。

快速路径——常量、恒等连接、运算符表达式（不求值，注册期展开为等价 Assign，读集自动收集去重）：

```cpp
req  = 0;                 // 常量：零读集
dout = cnt;               // 恒等连接
sum  = a + b;             // 算术/位/逻辑/比较/移位运算符均可
y    = 2 * (a & 0xffu) + 1;
```

`target` 可以是本模块的 `Wire`、`Out`，或直接子模块的 `In`。每根信号恰有一条驱动 Assign（SSA），elaboration 检查多驱动与悬空。

### 3.3 Update：时序逻辑

```cpp
reg.update().on(e1, e2, ...).en(g).reads(s1, s2, ...) = lambda;
```

- `.on(...)`：**必填**，边沿事件列表，`posedge(x)` / `negedge(x)`；`x` 可以是任意 `bool` 读源——输入端口、Wire（门控时钟）、`Reg<bool>`（分频器输出），时钟没有特殊地位
- `.en(g)`：**可选**电平守卫，`bool` 信号；守卫不满足时该 Update 不激活，状态保持。守卫对整条 Update 生效；需要分支级门控时（例如复位分支不受使能影响、计数分支受使能门控），把使能作为普通读集信号放进 lambda 自行判别
- `.reads(...)`：显式读集，语义同 Assign
- 快速路径：`reg.update().on(...) = 常量 / 信号 / 表达式;`

同一状态可挂多条 Update：同时激活时按**注册顺序定优先级**——先注册者胜（复位先于计数注册，复位即优先）。词序 `on → [en] → reads → lambda` 由 type-state builder 编译期强制。

### 3.4 Mem：按行寻址的存储器

`Mem<T, R>` 是 R 行 T 类型的状态阵列。写口比 Reg 的 Update 多一段 `.addr(...)`（必填，词序 `on → addr → [en] → reads`）：

```cpp
MEM(uint32_t, 1024, mem);   // 1024 行 32 位

mem.update().on(posedge(clk)).addr(waddr).en(wen).reads(wdata)
    = [](auto src) { auto [d] = src; return d; };          // 写口：激活时只写一行

rdata.assign().reads(mem, raddr) = [](auto src) {          // 读口：组合读 committed 内容
    auto [m, a] = src;
    return m[a];
};
```

行级提交：未写的行保持，提交成本与容量无关；多条 Update 写同一 Mem 时按注册序按行合并（异行互不干扰，同行先注册者胜）。写地址越界是运行时错误。

### 3.5 层次

子模块经 `SUB(T, name)` 创建，连线限定三种模式（父模块只能读写**直接**子模块的端口）：

```cpp
SUB(ShiftReg<Vec, 3>, sr);

sr.din = sorted;   // 下行：父模块 Assign 驱动子模块 In
sr.clk = clk;      // 时钟分发同理
dout = sr.dout;    // 上行：父模块读子模块 Out
// 兄弟连线 = 读一个子模块的 Out + 驱动另一个的 In，无需专门设施
```

跨级访问、读子模块内部信号、驱动自己的 `In`、用 Assign 驱动 `Reg/Mem`，均在注册时报错。

### 3.6 驱动与仿真

```cpp
top.elaborate("top");       // 根模块一次性：展平 → 结构检查 → 建图；之后结构冻结
top.clk.set(1);             // set：仅根模块 In，须在 elaborate 之后；只改输入不推进仿真
top.eval();                 // 推进到当前输入对应的稳态；幂等（输入不变重复调用状态不变）
auto v = top.dout.get();    // get：读任意实体当前值
```

`eval()` 内部按 round 循环：组合求值（含边沿检测，Update 只记意图）→ 状态更新（意图按优先级链统一生效，NBA）→ 有提交则进入下一 round（级联时钟由此同 eval 内推进），无提交则返回。round 数与组合环迭代数均有上限，超限报错（零延迟振荡 / 组合环不收敛）。

### 3.7 波形（FST）

Verilator 风格：`waveOn` 打开文件后，由用户显式推进时间、显式采样：

```cpp
top.waveOn("dump.fst");       // 须在 elaborate 之后
uint64_t time = 0;
for (...) {
    top.clk.set(0); top.eval(); top.waveDump(time++);
    top.clk.set(1); top.eval(); top.waveDump(time++);
}
top.waveOff();                // flush 并关闭
```

`waveDump(t)` 只记录值有变化的实体（首次调用全量记录初值）；时间必须非递减，回退报错。内建支持 `bool` 与整型（补码位串）；自定义类型特化 `FstFormat<T>`（静态成员 `bits` 与 `format(out, v)`）即可 dump；`Mem` 与无特化类型自动跳过。生成的 `.fst` 可用 GTKWave、surfer、wellen 等打开。

### 3.8 Trace 与调试开关

```cpp
top.traceOn(std::cout);       // round 级事件记录：每个 round 的边沿命中、
                              // 激活的 Update、提交的状态写（含值），供回溯
top.traceOff();

top.auditOn();                // 读集对账：lambda 经框架读路径访问未声明实体，
                              // 首次仿真即报错（需 -DWOLVICMOD_AUDIT=ON 构建）
top.auditOff();

top.assertUpdateMutexOn();    // 同一状态多条 Update 同 round 激活时报告
top.assertUpdateMutexOff();   // （默认关：注册序优先级静默生效，不视为错误）
```

---

## 4. 速查手册

### 声明宏

| 宏 | 实体 | 宏 | 实体 |
|---|---|---|---|
| `IN(T, n)` | 输入端口 | `REG(T, n)` | 寄存器 |
| `OUT(T, n)` | 输出端口 | `MEM(T, R, n)` | 存储器（R 行 T） |
| `WIRE(T, n)` | 组合信号 | `SUB(T, n)` | 子模块 |

### 注册链词序（编译期强制）

```cpp
wire/out/子in.assign().reads(s...) = lambda;          // Assign 完整形式
reg.update().on(e...)[.en(g)].reads(s...) = lambda;   // Update（Reg）
mem.update().on(e...).addr(a)[.en(g)].reads(s...) = lambda;  // Update（Mem）
target = 常量 | 信号 | 表达式;                         // Assign 快速路径
state.update().on(e...)[.en(g)] = 常量 | 信号 | 表达式; // Update 快速路径
```

### 根模块驱动

```cpp
top.elaborate("top");  一次
top.in.set(v);         仅根 In，elaborate 之后
top.eval();            推进到稳态，幂等
top.x.get();           观察任意实体
```

### 值类型与连线约束

| 约束 | 说明 |
|---|---|
| `T` 要求 | 可默认构造 + 可移动赋值；常量快速路径另需可拷贝构造 |
| 组合环中的信号 | 需要 `operator==`（稳态迭代判定） |
| 驱动规则 | 模块只能驱动自己的 Wire/Out 与直接子模块的 In；根 In 仅外部 `set()` |
| 读取规则 | 模块只能读自己的实体与直接子模块的端口 |
| 时钟采样 | 外部输入时钟在两次 `eval()` 之间至多跳变一次，否则边沿漏检 |
| 结构冻结 | 根模块构造返回后不可增删实体与动作 |

### 诊断一览（错误均为 `wolvicmod::Error`，附层次路径）

| 错误 | 检测点 |
|---|---|
| 多驱动 / 悬空 / 根 In 被驱动 | elaboration |
| 重名 / 未命名实体 | 创建时 / 注册时 |
| 连线规则违反（跨级、驱动 Reg 等） | 注册时 |
| 组合环不收敛、环类型无 `==` | 运行时 / elaboration |
| 零延迟振荡 | 运行时（round 上限） |
| Mem 写地址越界 | 运行时 |
| 偷读未声明信号 | 运行时（`auditOn`，需 AUDIT 构建） |
| 多 Update 同时激活 | 运行时（`assertUpdateMutexOn`，默认关） |
| 波形时间回退 | 运行时（`waveDump`） |

### CMake 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `WOLVICMOD_BUILD_TESTS` | 顶层构建时 ON | 构建 doctest 单测（CTest） |
| `WOLVICMOD_WITH_FST` | ON | FST 波形（vendored gtkwave/libfst，MIT） |
| `WOLVICMOD_AUDIT` | OFF | 编译读集对账插桩（`auditOn` 需要） |

---

## 仓库布局与许可

```
include/wolvicmod/   框架头文件（core/ elab/ sim/ dbg/ wave/）
tests/               doctest 单测
third_party/         doctest（单头）、libfst（gtkwave，MIT）
```

许可：MIT（本仓库）；第三方组件各附其许可。
