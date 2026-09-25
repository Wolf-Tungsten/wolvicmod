package refgen

import chisel3._
import _root_.circt.stage.ChiselStage

// 对拍参考源生成驱动。各模块的 wrapper 与配置表在 src/refs/ 下的独立文件中：
// QueueRef.scala / PipeRef.scala / ArbRef.scala。
object RefGen extends App {
  // XiangShan Makefile:109 的 firtool 选项（与 kunminghu-v3 emu 同流程）
  private val firtoolOpts = Array(
    "-O=release",
    "--disable-annotation-unknown",
    "--lowering-options=explicitBitcast,disallowLocalVariables,disallowPortDeclSharing,locationInfoStyle=none",
  )

  val outDir = args(0)
  val filter = if (args.length > 1) Some(args(1)) else None  // 名称子串过滤（run.sh 逐配置进程用）
  val firtoolPath = firtoolresolver.Resolve(chisel3.BuildInfo.firtoolVersion.get, true) match {
    case Right(bin) => bin.path.getAbsolutePath
    case Left(err)  => throw new RuntimeException(s"firtool resolve failed: $err")
  }
  println(s"[refgen] firtool: $firtoolPath")
  def emit(gen: => RawModule): Unit = {
    ChiselStage.emitSystemVerilogFile(
      gen,
      Array("--target-dir", outDir, "--firtool-binary-path", firtoolPath),
      firtoolOpts,
    )
  }
  def emitIf(name: String)(gen: => RawModule): Unit =
    if (filter.forall(name.contains)) emit(gen)

  private val all: Seq[(String, () => RawModule)] =
    refs.QueueRef.configs ++ refs.PipeRef.configs ++ refs.ArbRef.configs
  for ((name, gen) <- all) emitIf(name)(gen())
  println(s"[refgen] done -> $outDir")
}
