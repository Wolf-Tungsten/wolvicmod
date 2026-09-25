package refgen.refs

import chisel3._
import chisel3.util._

// chisel3.util.Queue 的参考 wrapper：端口拍平成简单 IO（Bool/UInt），
// 端口名即 Verilator 端口名，供 cosim harness 直接引用。
class QueueRef(val entries: Int, val pipe: Boolean, val flow: Boolean) extends Module {
  override def desiredName = s"QueueRef_e${entries}_p${if (pipe) 1 else 0}_f${if (flow) 1 else 0}"
  val enq_valid = IO(Input(Bool()))
  val enq_bits  = IO(Input(UInt(32.W)))
  val enq_ready = IO(Output(Bool()))
  val deq_valid = IO(Output(Bool()))
  val deq_bits  = IO(Output(UInt(32.W)))
  val deq_ready = IO(Input(Bool()))
  val count     = IO(Output(UInt(log2Ceil(entries + 1).W)))
  val q = Module(new Queue(UInt(32.W), entries, pipe = pipe, flow = flow))
  q.io.enq.valid := enq_valid
  q.io.enq.bits  := enq_bits
  enq_ready := q.io.enq.ready
  deq_valid := q.io.deq.valid
  deq_bits  := q.io.deq.bits
  q.io.deq.ready := deq_ready
  count := q.io.count
}

object QueueRef {
  // 对拍配置表：inj_buf(2,plain)、pipe queue(1,pipe)、eject oqueue(4,plain)、
  // flow(2)、flow+pipe(4)。（名称 = desiredName = SV 顶层模块名）
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("QueueRef_e1_p1_f0", () => new QueueRef(1, pipe = true,  flow = false)),
    ("QueueRef_e2_p0_f0", () => new QueueRef(2, pipe = false, flow = false)),
    ("QueueRef_e4_p0_f0", () => new QueueRef(4, pipe = false, flow = false)),
    ("QueueRef_e2_p0_f1", () => new QueueRef(2, pipe = false, flow = true)),
    ("QueueRef_e4_p1_f1", () => new QueueRef(4, pipe = true,  flow = true)),
  )
}
