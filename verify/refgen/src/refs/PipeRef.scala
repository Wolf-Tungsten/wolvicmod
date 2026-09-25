package refgen.refs

import chisel3._
import chisel3.util._

// chisel3.util.Pipe 的参考 wrapper：valid RegNext + bits RegEnable 的 N 拍延迟。
class PipeRef(val latency: Int) extends Module {
  override def desiredName = s"PipeRef_l$latency"
  val enq_valid = IO(Input(Bool()))
  val enq_bits  = IO(Input(UInt(32.W)))
  val deq_valid = IO(Output(Bool()))
  val deq_bits  = IO(Output(UInt(32.W)))
  val p = Module(new Pipe(UInt(32.W), latency))
  p.io.enq.valid := enq_valid
  p.io.enq.bits  := enq_bits
  deq_valid := p.io.deq.valid
  deq_bits  := p.io.deq.bits
}

object PipeRef {
  // 对拍配置表：latency = 1 / 3 / 5
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("PipeRef_l1", () => new PipeRef(1)),
    ("PipeRef_l3", () => new PipeRef(3)),
    ("PipeRef_l5", () => new PipeRef(5)),
  )
}
