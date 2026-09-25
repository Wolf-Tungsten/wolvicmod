#pragma once

// 预制菜（prefab）可复用时序元件库伞头文件。
//
// 全部元件是"用框架写的普通库模块"（namespace wolvicmod::prefab），不改动
// core/elab/sim 任何框架机制，不引入全局状态。收录标准：chisel3 标准库
// （chisel3.util）语义、框架通用——XiangShan 生态（xs-utils / dongjiang）
// 特有的元件属于项目侧（见 README §4 的边界说明）：
//   dec.h   —— Dec<T> 通道约定（valid+bits 合体，_rdy 独立端口）
//   queue.h —— Queue<T,N,Flow,Pipe>  ↔ chisel3.util.Queue
//   arb.h   —— FixedArb<T,N>         ↔ chisel3.util.Arbiter
//              RRArb<T,N>            ↔ chisel3.util.RRArbiter
//   pipe.h  —— ValidPipe<T,N>        ↔ chisel3.util.Pipe

#include "wolvicmod/prefab/arb.h"
#include "wolvicmod/prefab/dec.h"
#include "wolvicmod/prefab/pipe.h"
#include "wolvicmod/prefab/queue.h"
