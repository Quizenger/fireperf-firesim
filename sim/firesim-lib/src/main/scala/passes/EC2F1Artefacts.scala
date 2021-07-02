// See LICENSE for license details.

package firesim.passes

import firesim.util.{DesiredHostFrequency, BuildStrategy}
import midas.stage.phases.ConfigParametersAnnotation

import freechips.rocketchip.config.Parameters

import firrtl._
import firrtl.passes._
import firrtl.annotations._
import java.io.{File, FileWriter}

/** Generate additional output files for EC2 F1 host platforms, including TCL
  *  to configure a PLL with the desired frequency and to pick a desired
  *  build strategy
  */
object EC2F1Artefacts extends Transform {
  def inputForm: CircuitForm = LowForm
  def outputForm: CircuitForm = LowForm
  override def name = "[Golden Gate] EC2 F1 Artefact Generation"

  // Capture FPGA-toolflow related verilog defines
  def verilogHeaderAnno = GoldenGateOutputFileAnnotation(
    """| # Strips out Chisel's default $fatal emission which Vivado chokes on
       | `define SYNTHESIS
       | # Don't let Vivado see $random, which is the default if this is not set
       | `define RANDOM 64'b0
       |""".stripMargin,
    fileSuffix = "_defines.vh")

  // Emit TCL variables to control the FPGA compilation flow
  def tclEnvAnno(implicit hostParams: Parameters): GoldenGateOutputFileAnnotation =  {
    val requestedFrequency = hostParams(DesiredHostFrequency)
    val buildStrategy      = hostParams(BuildStrategy)
    val constraints = s"""# FireSim Generated Environment Variables
set desired_host_frequency ${requestedFrequency}
${buildStrategy.emitTcl}
"""
    GoldenGateOutputFileAnnotation(constraints, fileSuffix = "_env.tcl")
  }

  def execute(state: CircuitState): CircuitState = {
    implicit val p = state.annotations.collectFirst({ case ConfigParametersAnnotation(p)  => p }).get
    state.copy(annotations = verilogHeaderAnno +: tclEnvAnno +: state.annotations)
  }
}
