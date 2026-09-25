package refgen.refs

import chisel3._
import chisel3.util._

// chisel3.util.Arbiter（固定优先级，in(0) 最高）的参考 wrapper。
// 注：UInt 输出位选择赋值在 chisel7 只读，故 in_ready 用 Vec。
class FixedArbRef(val n: Int) extends Module {
  override def desiredName = s"FixedArbRef_n$n"
  val in_valid  = IO(Input(UInt(n.W)))
  val in_bits   = IO(Input(Vec(n, UInt(32.W))))
  val in_ready  = IO(Output(Vec(n, Bool())))
  val out_valid = IO(Output(Bool()))
  val out_bits  = IO(Output(UInt(32.W)))
  val out_ready = IO(Input(Bool()))
  val chosen    = IO(Output(UInt(log2Ceil(n).W)))
  val arb = Module(new Arbiter(UInt(32.W), n))
  for (i <- 0 until n) {
    arb.io.in(i).valid := in_valid(i)
    arb.io.in(i).bits  := in_bits(i)
    in_ready(i)        := arb.io.in(i).ready
  }
  out_valid := arb.io.out.valid
  out_bits  := arb.io.out.bits
  arb.io.out.ready := out_ready
  chosen := arb.io.chosen
}

// chisel3.util.RRArbiter（轮转优先级）的参考 wrapper。
class RRArbRef(val n: Int) extends Module {
  override def desiredName = s"RRArbRef_n$n"
  val in_valid  = IO(Input(UInt(n.W)))
  val in_bits   = IO(Input(Vec(n, UInt(32.W))))
  val in_ready  = IO(Output(Vec(n, Bool())))
  val out_valid = IO(Output(Bool()))
  val out_bits  = IO(Output(UInt(32.W)))
  val out_ready = IO(Input(Bool()))
  val chosen    = IO(Output(UInt(log2Ceil(n).W)))
  val arb = Module(new RRArbiter(UInt(32.W), n))
  for (i <- 0 until n) {
    arb.io.in(i).valid := in_valid(i)
    arb.io.in(i).bits  := in_bits(i)
    in_ready(i)        := arb.io.in(i).ready
  }
  out_valid := arb.io.out.valid
  out_bits  := arb.io.out.bits
  arb.io.out.ready := out_ready
  chosen := arb.io.chosen
}

object ArbRef {
  // 对拍配置表：固定/轮转各 N=4
  val configs: Seq[(String, () => RawModule)] = Seq(
    ("FixedArbRef_n4", () => new FixedArbRef(4)),
    ("RRArbRef_n4",    () => new RRArbRef(4)),
  )
}
