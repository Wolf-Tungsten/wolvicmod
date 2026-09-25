# wolvicmod/verify/ —— 通用预制菜 vs chisel3 标准库 RTL 的 Verilator 对拍

用真实 Chisel 源码（chisel 7.13.0，钉死 XiangShan kunminghu-v3 `build.mill` 的版本）
生成 SystemVerilog、Verilator 5.047 编译，与 wolvicmod 预制菜在相同激励下逐拍比对，
证明行为等价。**本目录完全自给自足**——只依赖 chisel ivy 包与 wolvicmod 自身，
不涉及任何项目仓/XiangShan 源码；不接入 wolvicmod 的 CMake 默认构建
（`add_subdirectory` 的消费者不受影响），纯 `run.sh` 驱动。

## 职责划分（与 tests/ 的关系）

| | `tests/`（单元测试） | `verify/`（本目录，RTL 对拍） |
|---|---|---|
| 定位 | 语义文档 + 快速回归 | 与真实 RTL 的等价性证明 |
| 依赖 | 纯 C++（doctest），无外部工具 | mill + chisel + firtool + Verilator |
| 耗时 | 毫秒级 | 分钟级 |
| 跑法 | `ctest`（每测试文件独立条目，`ctest -R <模块>` 单跑） | `./run.sh [模块]` |

同一模块的两套验证并列组织、各自可独立运行；预制菜任何语义改动应先过
`ctest` 再过本对拍。

## 怎么跑

```bash
# 推荐入口：wolvicmod 根目录的 Makefile
make -C wolvicmod cosim            # 全矩阵
make -C wolvicmod cosim M=queue    # 单模块（queue|pipe|arb）

# 或直接调脚本（支持更多开关）：
wolvicmod/verify/run.sh               # 全矩阵
wolvicmod/verify/run.sh queue         # 单模块（queue|pipe|arb）
wolvicmod/verify/run.sh --skip-refgen # SV 已生成时跳过 Chisel 阶段
wolvicmod/verify/run.sh --skip-build  # 只重跑对拍（复用已有二进制）
```

全部生成物在 `wolvicmod/build/verify/`（`build/` 已入 .gitignore），本目录只含源码。

依赖：mill launcher（`~/wksp/mill/mill`，`refgen/.mill-version` 钉 0.12.17）、
Verilator 5.047、OpenJDK 21。firtool 由 firtool-resolver 自动解析到缓存的
1.149.0（chisel 7.13.0 的 BuildInfo 指定版本）。

## 目录结构

```
verify/                # 只含源码；生成物在 ../build/verify/
├── refgen/            # 独立 mill 项目：Chisel 参考顶层生成器（只依赖 chisel 7.13.0）
│   └── src/           #   正规 chisel 项目布局：RefGen.scala 只做驱动，
│       ├── RefGen.scala        # 配置汇总/命令行解析/SV 输出
│       └── refs/               # 每模块一个文件：wrapper 定义 + 该模块配置表
│           ├── QueueRef.scala      # QueueRef_e{1,2,4}_p*_f* × 5 配置
│           ├── PipeRef.scala       # PipeRef_l{1,3,5} × 3 配置
│           └── ArbRef.scala        # FixedArbRef_n4 / RRArbRef_n4
├── cosim/             # C++ 对拍 harness（verilated 参考模型 + wolvicmod 预制菜）
│   ├── common.h       #   拍协议/失配报告（前 16 拍激励回放）/密度激励
│   └── harness_{queue,pipe,arb}.cpp
└── run.sh

../build/verify/       # 唯一生成物根：sv/<cfg>/、obj/<cfg>/、bin/、mill/、日志
                       # refgen/out 是指向它的符号链接（mill 0.12 无 --out-dir）
```

## 对拍矩阵（元件 × 参考 × 配置）

| 预制菜 | 参考（chisel3 标准库源码） | 配置 |
|---|---|---|
| `Queue` | `chisel3.util.Queue` | (N=1,pipe)、(2,plain)、(4,plain)、(2,flow)、(4,flow+pipe) |
| `ValidPipe` | `chisel3.util.Pipe` | latency=1、3、5 |
| `FixedArb` | `chisel3.util.Arbiter` | N=4 |
| `RRArb` | `chisel3.util.RRArbiter` | N=4 |

规模：10 配置 × 3 seed × 每 run 10~12 万拍。XiangShan 生态元件
（FastQueue/VipArb/QoS 仲裁/Alloc/SRAM 模板）的对拍在项目仓
`proj-xiangshan-l3/verify/`。

## 比对协议

- 两侧同一 PRNG（mt19937，固定 seed）逐拍生成激励，完全同步驱动。
- 拍协议：驱动本拍输入 → `clk=0 eval`（组合稳态）→ 采样比对全部输出端口 →
  `clk=1 eval`（提交）。
- 参考侧先复位 4 拍（输入清零），撤复位后第 0 拍开始比对；wolvicmod 侧初始态
  即复位后态（不加任何 RANDOMIZE define：Verilator 两态零初始化）。
- 激励：定向相位（灌满→排空振荡、满时/空时同拍推拉、全 valid 连发与反压、
  单路轮换）+ 随机密度分段（100%/50%/10%）。
- 每拍比对所有输出端口；bits 仅在 valid 时比对。
- 失配报告：元件、配置、seed、拍号、端口（含 lane 号）、两侧值、前 16 拍激励
  回放；每 run 最多报 5 条，计数不停。

## 已知语义收窄点

1. **无效时的 bits 不比对**：valid=0 时数据通路取值属 don't-care。
2. **Queue flow 的空直通**：chisel 源码在被消费拍强制 do_enq/do_deq 均 false
   （无幻影拷贝），两侧指针/ram 轨迹一致，可观测行为等价。
3. **chisel Arbiter/RRArbiter 的 in.ready 不门控自身 valid**：`ready[i] =
   out.ready && 没有更高优先候选在我前面`——低于首个 valid 的无效路也 ready。
   不会多 fire（它们无效），但端口波形不同——对拍实证后按此实现。

## 对拍中抓到并修复的预制菜 bug

| bug | 根因 | 修法 |
|---|---|---|
| `FixedArb`/`RRArb` in_rdy 门控自身 valid | chisel Arbiter/RRArbiter 的 ready 不门控 valid（生成 SV 实证） | 改为"无更高优先候选在前"的两遍结构 |
