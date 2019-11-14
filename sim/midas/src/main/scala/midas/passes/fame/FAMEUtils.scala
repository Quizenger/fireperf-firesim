// See LICENSE for license details.

package midas.passes.fame

import firrtl._
import ir._
import Utils._
import Mappers._
import traversals.Foreachers._
import graph.DiGraph
import analyses.InstanceGraph
import transforms.CheckCombLoops

import annotations._

import scala.collection
import collection.mutable
import collection.mutable.{LinkedHashSet, LinkedHashMap, MultiMap}

object RTRenamer {
  // TODO: determine order for multiple renames, or just check of == 1 rename?
  def exact(renames: RenameMap): (ReferenceTarget => ReferenceTarget) = {
    { rt =>
      val renameMatches = renames.get(rt).getOrElse(Seq(rt)).collect({ case rt: ReferenceTarget => rt })
      assert(renameMatches.length == 1)
      renameMatches.head
    }
  }

  def apply(renames: RenameMap): (ReferenceTarget => Seq[ReferenceTarget]) = {
    { rt => renames.get(rt).getOrElse(Seq(rt)).collect({ case rt: ReferenceTarget => rt }) }
  }
}

private[fame] object FAMEChannelAnalysis {
  def removeCommonPrefix(a: String, b: String): (String, String) = (a, b) match {
    case (a, b) if (a.length == 0 || b.length == 0) => (a, b)
    case (a, b) if (a.charAt(0) == b.charAt(0)) => removeCommonPrefix(a.drop(1), b.drop(1))
    case (a, b) => (a, b)
  }

  def getHostDecoupledChannelPayloadType(name: String, ports: Seq[Port]): Type = {
    val fields = ports.map(p => Field(removeCommonPrefix(p.name, name)._1, Default, p.tpe))
    if (fields.size > 1) {
      new BundleType(fields.toSeq)
    } else {
      fields.head.tpe
    }
  }

  def getHostDecoupledChannelType(name: String, ports: Seq[Port]): Type = Decouple(getHostDecoupledChannelPayloadType(name, ports))
}

private[fame] class FAMEChannelAnalysis(val state: CircuitState, val fameType: FAMETransformType) {
  // TODO: only transform submodules of model modules
  // TODO: add renames!
  val circuit = state.circuit
  val topTarget = ModuleTarget(circuit.main, circuit.main)
  def moduleTarget(m: DefModule) = topTarget.copy(module = m.name)
  def moduleTarget(wi: WDefInstance) = topTarget.copy(module = wi.module)

  // The presence of a clock port is used to indicate whether a module contains state
  def stateful(m: DefModule) = m.ports.exists(_.tpe == ClockType)

 /*
  * A list of stateful modules that doesn't include blackboxes.
  * This is needed since only pure-FIRRTL modules get transformed.
  * Blackboxes' ports remain unchanged, as does their Verilog source.
  */
  val syncNativeModules = circuit.modules.collect({ case m: Module if stateful(m) => moduleTarget(m) }).toSet

  val moduleNodes = new LinkedHashMap[ModuleTarget, DefModule]
  val portNodes = new LinkedHashMap[ReferenceTarget, Port]
  circuit.modules.foreach({
    m => {
      val mTarget = ModuleTarget(circuit.main, m.name)
      moduleNodes(mTarget) = m
      m.ports.foreach({
        p => portNodes(mTarget.ref(p.name)) = p
      })
    }
  })

  // Used to check if any modules contains synchronous blackboxes in its sub-hierarchy
  private val syncBlackboxes = circuit.modules.collect({ case bb: ExtModule if stateful(bb) => bb.name }).toSet
  lazy val iGraph = new InstanceGraph(circuit)
  lazy val mGraph = iGraph.graph.transformNodes[String](_.module)
  def containsSyncBlackboxes(m: Module) = mGraph.reachableFrom(m.name).exists(syncBlackboxes.contains(_))

  lazy val connectivity = (new CheckCombLoops).analyze(state)

  val channels = new LinkedHashSet[String]
  val channelsByPort = new LinkedHashMap[ReferenceTarget, String]
  val transformedModules = new LinkedHashSet[ModuleTarget]
  state.annotations.collect({
    case fta @ FAMETransformAnnotation(tpe, mt) if (tpe == fameType) =>
      transformedModules += mt
    case fca: FAMEChannelConnectionAnnotation =>
      channels += fca.globalName
      fca.clock.foreach({ rt => channelsByPort(rt) = fca.globalName })
      fca.sinks.toSeq.flatten.foreach({ rt => channelsByPort(rt) = fca.globalName })
      fca.sources.toSeq.flatten.foreach({ rt => channelsByPort(rt) = fca.globalName })
  })

  private val moduleOfInstance = new LinkedHashMap[String, String]
  val topConnects = new LinkedHashMap[ReferenceTarget, ReferenceTarget]
  val inputChannels = new LinkedHashMap[ModuleTarget, mutable.Set[String]] with MultiMap[ModuleTarget, String]
  val outputChannels = new LinkedHashMap[ModuleTarget, mutable.Set[String]] with MultiMap[ModuleTarget, String]
  getTopConnects(moduleNodes(topTarget).asInstanceOf[Module].body)

  def getTopConnects(stmt: Statement): Unit = stmt match {
    case WDefInstance(_, iname, mname, _) =>
      moduleOfInstance(iname) = mname
    case Connect(_, WRef(tpname, ClockType, _, _), WSubField(WRef(iname, _, _, _), pname, ClockType, _)) =>
      // Clock connect, don't make any channels
      // The clock in a FAMEChannelConnectionAnnotation is the clock from model to bridge
      val tpRef = topTarget.ref(tpname)
      val child = topTarget.instOf(iname, moduleOfInstance(iname))
      topConnects(tpRef) = child.ref(pname)
    case Connect(_, WRef(tpname, _, _, _), WSubField(WRef(iname, _, _, _), pname, _, _)) =>
      val tpRef = topTarget.ref(tpname)
      channelsByPort.get(tpRef).foreach({ cname =>
        val child = topTarget.instOf(iname, moduleOfInstance(iname))
        topConnects(tpRef) = child.ref(pname)
        outputChannels.addBinding(child.ofModuleTarget, cname)
      })
    case Connect(_, WSubField(WRef(iname, _, _, _), pname, _, _), WRef(tpname, _, _, _)) =>
      val tpRef = topTarget.ref(tpname)
      channelsByPort.get(tpRef).foreach({ cname =>
        val child = topTarget.instOf(iname, moduleOfInstance(iname))
        topConnects(tpRef) = child.ref(pname)
        inputChannels.addBinding(child.ofModuleTarget, cname)
      })
    case s => s.foreach(getTopConnects)
  }

  val transformedSinks = new LinkedHashSet[String]
  val transformedSources = new LinkedHashSet[String]
  val sinkModel = new LinkedHashMap[String, InstanceTarget]
  val sourceModel = new LinkedHashMap[String, InstanceTarget]

  // clock ports don't go from one model to the other -> only one map needed
  val modelClockPort = new LinkedHashMap[String, Option[ReferenceTarget]]
  val sinkPorts = new LinkedHashMap[String, Seq[ReferenceTarget]]
  val sourcePorts = new LinkedHashMap[String, Seq[ReferenceTarget]]
  val staleTopPorts = new LinkedHashSet[ReferenceTarget]
  state.annotations.collect({
    case fca: FAMEChannelConnectionAnnotation =>
      channels += fca.globalName

      // Clock port always gets recorded and marked for deletion in FAME transform
      modelClockPort(fca.globalName) = fca.clock
      staleTopPorts ++= fca.clock

      val sinks = fca.sinks.toSeq.flatten
      sinkPorts(fca.globalName) = sinks
      sinks.headOption.filter(rt => transformedModules.contains(ModuleTarget(rt.circuit, topConnects(rt).encapsulatingModule))).foreach({ rt =>
        assert(!topConnects(rt).isLocal) // need instance info
        sinkModel(fca.globalName) = topConnects(rt).targetParent.asInstanceOf[InstanceTarget]
        transformedSinks += fca.globalName
        staleTopPorts ++= sinks
      })

      val sources = fca.sources.toSeq.flatten
      sourcePorts(fca.globalName) = sources
      sources.headOption.filter(rt => transformedModules.contains(ModuleTarget(rt.circuit, topConnects(rt).encapsulatingModule))).foreach({ rt =>
        assert(!topConnects(rt).isLocal) // need instance info
        sourceModel(fca.globalName) = topConnects(rt).targetParent.asInstanceOf[InstanceTarget]
        transformedSources += fca.globalName
        staleTopPorts ++= sources
      })
  })

  val hostClock = state.annotations.collect({ case FAMEHostClock(rt) => rt }).head
  val hostReset = state.annotations.collect({ case FAMEHostReset(rt) => rt }).head

  private def irPortFromGlobalTarget(rt: ReferenceTarget): Port = {
    portNodes(topConnects(rt).pathlessTarget)
  }

  def portsByInputChannel(mTarget: ModuleTarget): Map[String, (Option[Port], Seq[Port])] = {
    val iChannels = inputChannels.get(mTarget).toSet.flatten
    iChannels.map({
      cname => (cname, (modelClockPort(cname).map(irPortFromGlobalTarget), sinkPorts(cname).map(irPortFromGlobalTarget)))
    }).toMap
  }

  def portsByOutputChannel(mTarget: ModuleTarget): Map[String, (Option[Port], Seq[Port])] = {
    val oChannels = outputChannels.get(mTarget).toSet.flatten
    oChannels.map({
      cname => (cname, (modelClockPort(cname).map(irPortFromGlobalTarget), sourcePorts(cname).map(irPortFromGlobalTarget)))
    }).toMap
  }

  lazy val modelPorts = {
    val mPorts = new LinkedHashMap[ModuleTarget, mutable.Set[FAMEChannelPortsAnnotation]] with MultiMap[ModuleTarget, FAMEChannelPortsAnnotation]
    state.annotations.collect({
      case fcp @ FAMEChannelPortsAnnotation(_, _, port :: ps) => mPorts.addBinding(port.moduleTarget, fcp)
    })
    mPorts
  }

  // Looks up all FAMEChannelPortAnnotations bound to a model module, to generate a Map
  // from channel name to clock option and port list
  private def genModelChannelPortMap(direction: Option[Direction])(mTarget: ModuleTarget): Map[String, (Option[Port], Seq[Port])] = {
    modelPorts(mTarget).collect({
      case FAMEChannelPortsAnnotation(name, clock, ports) if direction == None || portNodes(ports.head).direction == direction.get =>
        (name, (clock.map(portNodes(_)), ports.map(portNodes(_))))
    }).toMap
  }

  def modelInputChannelPortMap: ModuleTarget => Map[String, (Option[Port], Seq[Port])]  = genModelChannelPortMap(Some(Input))
  def modelOutputChannelPortMap: ModuleTarget => Map[String, (Option[Port], Seq[Port])] = genModelChannelPortMap(Some(Output))
  def modelChannelPortMap: ModuleTarget => Map[String, (Option[Port], Seq[Port])]       = genModelChannelPortMap(None)

  def getSinkHostDecoupledChannelType(cName: String): Type = {
    FAMEChannelAnalysis.getHostDecoupledChannelType(cName, sinkPorts(cName).map(portNodes(_)))
  }

  def getSourceHostDecoupledChannelType(cName: String): Type = {
    FAMEChannelAnalysis.getHostDecoupledChannelType(cName, sourcePorts(cName).map(portNodes(_)))
  }

  // Coalesces channel connectivity annotations to produce a single port list
  // - Used to produce port annotations in InferModelPorts
  // - Reran to look up port names on model instances
  class ModulePortDeduper(val mTarget: ModuleTarget) {
    val channelDedups = new LinkedHashMap[String, String]

    private val visitedLeafPort = new LinkedHashSet[Port]()
    private val visitedChannel = new LinkedHashMap[(Option[Port], Seq[Port]), String]()

    private def channelIsDuplicate(ps: (Option[Port], Seq[Port])): Boolean = visitedChannel.contains(ps)
    private def channelSharesPorts(ps: (Option[Port], Seq[Port])): Boolean = ps match {
      case (clk, ports) => ports.exists(visitedLeafPort(_)) // clock can be shared
    }

    private def dedupPortLists(pList: Map[String, (Option[Port], Seq[Port])]): Map[String, (Option[Port], Seq[Port])] = pList.flatMap({
      case (cName, (_, Nil)) => throw new RuntimeException(s"Channel ${cName} is empty (has no associated ports)")
      case (_, clockAndPorts) if channelSharesPorts(clockAndPorts) && !channelIsDuplicate(clockAndPorts) =>
        throw new RuntimeException("Channel definition has partially overlapping ports with existing channel definition")
      case (cName, clockAndPorts) if channelIsDuplicate(clockAndPorts) =>
        channelDedups(cName) = visitedChannel(clockAndPorts)
        None
      case (cName, (clock, ports)) =>
        visitedChannel((clock, ports)) = cName
        visitedLeafPort ++= clock
        visitedLeafPort ++= ports
        channelDedups(cName) = cName
        Some(cName, (clock, ports))
      }).toMap

    private val inputPortMap = dedupPortLists(portsByInputChannel(mTarget))
    private val outputPortMap = dedupPortLists(portsByOutputChannel(mTarget))

    val completePortMap = inputPortMap ++ outputPortMap
  }

  lazy val modulePortDedupers = transformedModules.map((mT: ModuleTarget) => new ModulePortDeduper(mT))
  lazy val portDedups: Map[ModuleTarget, Map[String, String]] =
    modulePortDedupers.map(mD => mD.mTarget -> mD.channelDedups.toMap).toMap

  // Generates WSubField node pointing at a model instance from the channel name
  // and an iTarget to  model instance
  private def wsubToPort(modelInstLookup: String => InstanceTarget, suffix: String)
                        (cName: String): WSubField = {
    val modelInst = modelInstLookup(cName)
    val portName = portDedups(modelInst.ofModuleTarget)(cName)
    WSubField(WRef(modelInst.instance), s"${portName}${suffix}"),
  }

  def wsubToSinkPort: String => WSubField = wsubToPort(sinkModel, "_sink")
  def wsubToSourcePort: String => WSubField = wsubToPort(sourceModel, "_source")
}
