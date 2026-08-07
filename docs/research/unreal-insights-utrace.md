# Unreal Insights, the `.utrace` format, and the transferability of our average-frame profiler

Research report — feasibility analysis. No library code changes proposed here.

**Provenance and versions.** Source citations are from the `EpicGames/UnrealEngine` `release` branch (read via `gh`, account has access), which at time of writing tracks **UE 5.6-era `release`**; docs citations are **UE 5.8** ("Developer Guide to Tracing", "Timing Insights", "Task Graph Insights", "Using the Timers and Counters tabs"). Where a claim is read from source I cite the file path; where it is read from docs I link the page; where it is my own reasoning I label it **INFERENCE**. The wire format is versioned (`EProtocol::Id` is currently **7**, see §2), so exact byte layouts below are Protocol-5/6/7 and will not match a 4.x capture.

Our own profiler, for comparison throughout: `tools/graph_trace.h` runs a **fixed, build-once/run-many** `Static_task_graph` many times and aggregates per-node/per-edge/per-frame statistics into a single **average-frame** SVG (Welford mean/var, P² P50/P95, min/max, dispatch-wait, critical-path *frequency*, makespan, dead time, core utilization). The identity of a node is stable across runs and the dependency edges are known a priori. Keep that shape in mind — most of the transferability question turns on it.

---

## 1. Unreal Insights architecture

### 1.1 The four layers

Insights is the primary consumer of a general-purpose structured-logging framework called **Trace**. The pipeline, runtime → UI:

1. **`TraceLog`** (`Engine/Source/Runtime/TraceLog/`) — the runtime emit side. Header-only-ish macro layer plus a private writer. Defines events (`UE_TRACE_EVENT_BEGIN/_FIELD/_END`), channels (`UE_TRACE_CHANNEL`), and the log macro (`UE_TRACE_LOG`). Events are serialized into **thread-local buffers**, and a worker thread drains those buffers, frames them into packets, optionally LZ4-compresses, and ships them (file or socket). This is the module that defines the wire protocol (`Public/Trace/Detail/Protocol.h`, §2).
2. **`TraceAnalysis`** (`Engine/Source/Developer/TraceAnalysis/`) — the decode side. Reads the byte stream, undoes transport framing (`Private/Analysis/Transport/TidPacketTransport.cpp`), reconstructs the per-thread event streams, and dispatches decoded events to registered *analyzers*. `TraceAnalysis` is effectively the **de-facto spec** for the format (there is no separate document; see §2.5).
3. **`TraceServices`** (`Engine/Source/Developer/TraceServices/`) — the analyzed **data model**. Analyzers populate *providers* (`Public/TraceServices/Model/`), each a queryable in-memory model: a timing provider (per-thread timeline of CPU/GPU scopes), a frames provider, a counters provider, a tasks provider, a memory/allocations provider, a net provider, etc. This is the layer a headless tool would query (§5–6).
4. **`TraceInsights` / `TraceInsightsCore`** (`Engine/Source/Developer/TraceInsights/`) — the Slate UI: **Timing Insights** (per-thread CPU/GPU timelines, Frames panel, Timers/Counters aggregation tabs), **Tasks Insights** (task-graph relations + critical path), **Memory**, **Networking**, **Asset Loading**. Also hosts the headless CSV exporters (`Private/Insights/TimingProfiler/ViewModels/TimingExporter.{h,cpp}`, §5.1).

Server-side plumbing between (1) and (2): the **Unreal Trace Server** (`Trace Recorder` listens on **port 1981**; `Trace Store` writes/watches `.utrace` files). ([Trace in UE5](https://dev.epicgames.com/documentation/en-us/unreal-engine/trace-in-unreal-engine-5))

### 1.2 How events are emitted (channels, scopes, frames)

- **Channels** gate emission. `UE_TRACE_CHANNEL(Name)`, disabled by default, opted in with `-trace=default,cpu,frame,gpu,task,...`. An event tied to a channel is dropped entirely unless the channel is on. Cited: `Engine/Source/Runtime/Core/Public/Async/TaskTrace.h` declares `UE_TRACE_CHANNEL_EXTERN(TaskChannel, ...)`; `MiscTrace.h` declares `BookmarkChannel`; `CpuProfilerTrace.h` declares `CpuChannel`. ([Developer Guide to Tracing](https://dev.epicgames.com/documentation/en-us/unreal-engine/developer-guide-to-tracing-in-unreal-engine))
- **CPU timing scopes** are the workhorse. `TRACE_CPUPROFILER_EVENT_SCOPE(Name)` / `..._STR("literal")` open a scoped timer. Mechanically (`Engine/Source/Runtime/Core/Public/ProfilingDebugging/CpuProfilerTrace.h`): a timer name is registered **once** to obtain a `SpecId` (`FCpuProfilerTrace::GetOrCreateSpecId`, `OutputEventMetadataSpec`), then each entry emits `OutputBeginEvent(SpecId)` and each exit `OutputEndEvent()`. Begin carries the SpecId + a timestamp; End carries only a timestamp. There are dynamic-name variants (`OutputBeginDynamicEvent`) at extra cost, and `OutputResumeEvent`/`OutputSuspendEvent` for fibers/coroutines. The **name↔id table is emitted inline** in the same stream, so the decoder learns timer names as it goes.
- **Frames.** `Engine/Source/Runtime/Core/Public/ProfilingDebugging/MiscTrace.h` defines `enum ETraceFrameType { TraceFrameType_Game, TraceFrameType_Rendering, TraceFrameType_Count }` and `FMiscTrace::OutputBeginFrame(FrameType)` / `OutputEndFrame(FrameType)` (macros `TRACE_BEGIN_FRAME` / `TRACE_END_FRAME`). **A "frame" in Insights is exactly a Begin/End pair on one of these two timelines** — Game and Rendering are tracked separately. The Frames panel bars are the wall-clock durations of these pairs.
- **Bookmarks / Regions** (`MiscTrace.h`): `TRACE_BOOKMARK(...)` emits a named instant; `OutputBeginRegion/OutputEndRegion(Name)` bracket a named span that can later scope an export (`-region=` in §5.1). Bookmarks/regions are for state changes rather than hot timing.
- **Counters** (`ProfilingDebugging/CountersTrace.h`, referenced in docs): `TRACE_DECLARE_INT_COUNTER`, `TRACE_COUNTER_SET/INCREMENT/ADD` — time-series scalar values, shown in the Counters tab.
- **Tasks** (`Async/TaskTrace.h`): a full DAG-event vocabulary — `Created`, `Launched(DebugName, ThreadToExecuteOn, ...)`, `Scheduled`, **`SubsequentAdded(TaskId, SubsequentId)`** (this is the *edge*), `Started`, `Finished`, `Completed`, `Destroyed`, plus `FWaitingScope` and `FTaskTimingEventScope`. `TaskTraceVersion = 1`. This channel is what makes a *real dependency graph* recoverable from a capture — see §4.3.

### 1.3 What "profile" means here

Insights captures a **per-thread event stream**: nested CPU scopes per worker/game/render thread, GPU tracks, task events, counters, memory ops — all timestamped against a single clock (`NewTrace` event carries `StartCycle` + `CycleFrequency`, `Writer.cpp` §2.2). The UI's default mode is **inspect one capture**: scrub the timeline, select a frame or a range, drill into nesting. Aggregation exists but is **range-scoped and flat** (§3), not a fixed-graph average.

---

## 2. The `.utrace` file format

The format is a **stream** of the exact bytes `TraceLog`'s writer produces; a `.utrace` file is that stream captured to disk (the same bytes go over the socket to the Trace Recorder). The companion **`.ucache`** is derived/indexed data Insights builds next to the file — not part of the wire format.

### 2.1 Stream header / handshake

From `Engine/Source/Runtime/TraceLog/Private/Trace/Writer.cpp` (`Writer_DescribeEvents`/handshake path):

```
FHandshake:
  uint32 Magic          = 'T''R''C''2'   // bytes: 'T','R','C','2'  ('2' | 'C'<<8 | 'R'<<16 | 'T'<<24)
  uint16 MetadataSize
  uint16 MetadataField0 = sizeof(ControlPort) | (ControlPortFieldId<<8)
  uint16 ControlPort
  uint16 MetadataField1 = sizeof(FTraceGuid) | (SessionGuidFieldId<<8)
  uint8  SessionGuid[16]
  uint16 MetadataField2 = sizeof(FTraceGuid) | (TraceGuidFieldId<<8)
  uint8  TraceGuid[16]
Stream header (immediately after):
  uint8  TransportVersion = ETransport::TidPacketSync (=4)
  uint8  ProtocolVersion  = EProtocol::Id             (=7 currently)
```

Then the first logged event is **`NewTrace`** (`Writer.cpp`): `StartCycle`, `CycleFrequency` (the timer→seconds conversion), `Endian` marker `0x524d`, `PointerSize`, `StartDateTime`. So the very first thing a decoder learns is the clock and word size.

**INFERENCE:** the magic literal is `"TRC2"` (the `'2'` is the low byte). Older captures used a different fourth byte; the transport/protocol version bytes are the reliable discriminators, not the magic.

### 2.2 Transport framing (TID packets)

`Engine/Source/Runtime/TraceLog/Public/Trace/Detail/Transport.h` + decoder `Engine/Source/Developer/TraceAnalysis/Private/Analysis/Transport/TidPacketTransport.cpp`:

```
FTidPacketBase { uint16 PacketSize; uint16 ThreadId; }   // sizeof == 4
  ThreadId bits: EncodedMarker=0x8000 (LZ4-compressed payload),
                 Verification=0x4000 (payload followed by 64-bit serial), ThreadIdMask=0x3fff
TTidPacketEncoded adds: uint16 DecodedSize; then Data[]    // sizeof base == 6
```

- The stream is a sequence of packets. Each packet belongs to **one thread id** (`ThreadId & 0x3fff`). The decoder demultiplexes packets into **per-thread byte streams** (`FThreadStream` per tid) and each thread stream is then parsed independently for events.
- Reserved pseudo-thread ids (`ETransportTid`): `Events=0`, `Importants/Internal=1`, `Bias=2` (real threads start at Bias), `Sync=0x3fff`, `PseudoImportants=0x3ffe`. "Important" events (event-type definitions, cached lifetime events) live on their own stream so they survive/replay for any connection.
- If `EncodedMarker` is set, `Packet->Data` is **LZ4-compressed** (`Decode(...)` → `Thread->Buffer.Append(DecodedSize)`); `DecodedSize` is the pre-header field. Small packets are shipped uncompressed. This confirms the docs' "packets, LZ4-compressed unless too small".
- `ETransport` enum: `Raw=1, Packet=2, TidPacket=3, TidPacketSync=4(active)`. The active transport for `.utrace` is **TidPacketSync**.

### 2.3 Event encoding within a thread stream

`Engine/Source/Runtime/TraceLog/Public/Trace/Detail/Protocols/Protocol{0..7}.h`. Protocol0 defines the field type system; later versions reuse and extend it.

- **Field type system** (`Protocol0::EFieldType`): a byte packing category (Integer/Float/Array), size (`Pow2Size` 8/16/32/64), and specials (Pod/String/Signed). `AnsiString`/`WideString`/`Array` are variable-length via an **aux-data** mechanism.
- **Event-type registration** (`FNewEventEvent`, UID `NewEvent=0`): a self-describing schema — `EventUid`, `FieldCount`, `Flags`, logger-name + event-name sizes, and a `Fields[]` array of `{Offset, Size, TypeInfo, NameSize}` followed by the name bytes. **Every event type is described in-band before use**; the decoder builds its schema table from the stream. This is why the format is "self-describing / compatible across engine versions" (docs).
- **Event instances** (`Protocol5::FEventHeader { uint16 Uid; Data[] }`, 2 bytes). Synchronized events use `FEventHeaderSync { uint16 Uid; uint16 SerialLow; uint8 SerialHigh; ... }` (5 bytes, a 24-bit serial for cross-thread ordering). "Important" events use `FImportantEventHeader { Uid; Size; Data[] }` (4 bytes).
- **Variable-length fields** use `FAuxHeader` (4 bytes: `Uid`, `FieldIndex_Size`, `Size`, then `Data[]`) — strings/arrays are emitted as trailing aux blocks referencing a field index, terminated by `AuxDataTerminal`.
- **Well-known UIDs** (`Protocol7::EKnownEventUids`): `NewEvent=0`, `AuxData`, `AuxDataTerminal`, `EnterScope`, `LeaveScope`, and the version-7 additions **`EnterScope_TA`/`LeaveScope_TA`** (absolute timestamps) and **`EnterScope_TB`/`LeaveScope_TB`** (timestamps relative to a `BaseTimestamp`). Low bit `Flag_TwoByteUid` selects 1- vs 2-byte UID encoding; `_UidShift=1`.
- **Integers are 7-bit LEB128 / zig-zag** for compactness: `FTraceUtils::Encode7bit` and `EncodeZigZag` in `MiscTrace.h`. So field values are not raw fixed-width on the wire in the common path.

**The CPU scope on the wire** is therefore: a one-time `NewEvent` describing the CPU-event spec + a `EventSpec`/metadata event mapping SpecId→name, then per instance an `EnterScope*`(SpecId, timestamp) and a matching `LeaveScope*`(timestamp), all inside that thread's packet stream. Timestamps are delta-encoded (`_TB` relative to a base) to keep them small.

### 2.4 Version stability

- **Protocol is explicitly versioned 0→7** (`Protocol.h` includes all eight; `EProtocol::Id` is the active one, **7**). New versions *reuse* prior structs (`Protocol7` does `using Protocol6::...`; Protocol6 reuses Protocol5). The churn 5→6→7 is mostly **timestamp representation** (`EnterScope_T` → `_TA`/`_TB`). A decoder must branch on `ProtocolVersion` from the stream header.
- **Transport is separately versioned** (`ETransport`, active `TidPacketSync=4`).
- **Self-describing schemas** mean *new event types* don't break an old decoder (it can skip unknown UIDs by `Size`), but *new protocol/transport versions* (framing, timestamp encoding, UID width) **do** require decoder updates. **FACT:** Epic states traces are "compatible with different engine release versions" — true at the container level, but a hand-rolled decoder is pinned to the protocol versions it implements.

### 2.5 How documented is it?

**There is no published byte-level spec.** Epic documents the *concepts* (channels, transport-is-packets, LZ4, self-describing events) but not the struct layouts. **`TraceAnalysis` is the authoritative spec** — the structs in `Trace/Detail/Protocols/Protocol*.h` and the decode in `TidPacketTransport.cpp` / `Analysis/Processor.cpp` / `Analysis/StreamReader.cpp` are what you must read. Community reverse-engineering exists but is thin and version-lagged. **Confidence: high** for the layouts quoted above (read directly from source); **medium** for cross-version details on 4.x captures (I read `release`, not older tags).

---

## 3. What Insights profiles + how it visualizes — vs our model

### 3.1 Insights' model: event-stream, single-capture-inspect

The native model is a **timeline of nested scopes per thread** plus parallel tracks (GPU, tasks, counters, frames). Core interactions ([Timing Insights](https://dev.epicgames.com/documentation/unreal-engine/timing-insights-in-unreal-engine-5)):

- **Frames panel**: a bar per frame (Game or Rendering), bar height = frame wall time. Click a frame → zoom the Timing view to it.
- **Timing view**: per-thread lanes, nested scope bars, width = duration. Hover = one instance's stats.
- **Tasks Insights** ([Task Graph Insights](https://dev.epicgames.com/documentation/unreal-engine/task-graph-insights-in-unreal-engine-5)): for a *selected task*, draws relations across its prerequisites/subsequents through the created→launched→scheduled→started→finished→completed stages, and — with the **Critical Path** option — "the execution chain in the current task's graph that has the longest execution time" (component time = `FinishedTime − StartTime`).

### 3.2 The one place Insights aggregates: the Timers/Counters tabs

([Using the Timers and Counters tabs](https://dev.epicgames.com/documentation/unreal-engine/using-the-timers-and-counters-tabs-in-unreal-engine)) Select a frame or Ctrl-drag a range; the **Timers tab** aggregates *every instance of each timer in that range* into a flat table: `Instance Count`, and `Total / Max / Average / Median / Min` for both **Inclusive** and **Exclusive** time. Exportable to `.csv/.tsv` (Ctrl+S). Counters tab similarly.

### 3.3 The contrast, sharply

| Dimension | Our profiler | Insights |
|---|---|---|
| Unit of identity | **Graph node** (stable across runs; declared) | **Timer name / SpecId** (stable) *and* **task id** (per-instance) |
| Repetition model | **run-many of a fixed DAG**, aligned by node | one capture; many frames *in* it, not aligned |
| Aggregation | per-node + per-edge + frame, **cross-run** | per-timer flat table over a **selected range** |
| Dependency structure | **explicit edges**, always present | only if **task** channel on; else just scope *nesting* |
| Critical path | measured per run → **frequency** over runs | per **selected task**, single capture, longest-time chain |
| Dead time / utilization | first-class frame metrics | derivable by hand from thread busy vs wall; not a headline |
| "Average frame" layout | **yes** — the whole output | **no** |

**Key finding (answers the framing question):** Insights has **range-scoped statistical aggregation** (the Timers tab) but **no "average frame" concept** — no notion of aligning frame *N*'s scopes to frame *N+1*'s and folding them into one representative frame with per-node variance, criticality frequency, and a packed concurrency layout. Its aggregation is *flat over a time window*; ours is *structural over repetitions of a known graph*. That gap is exactly the thing our tool does that Insights does not — and §4 is whether our thing can be *fed by* Insights data.

---

## 4. Transferability of our average-frame approach to Insights data

Question: given a `.utrace` of many frames, could you produce *our* kind of output? Component by component.

### 4.1 Frame segmentation — **transfers cleanly**
`OutputBeginFrame/OutputEndFrame` give exact Game/Rendering frame boundaries. You get a clean sequence of frames to fold. **Caveat:** pick *one* frame timeline (Game) as the fold unit; Game and Render frames are independent and phase-shifted (the render thread runs a frame behind — which our own sample models with `Versioned<Transforms>`), so you cannot naively union them.

### 4.2 Stable scope identity for cross-frame aggregation — **transfers, but fuzzy**
Timers are named + id'd (SpecId), so "the same timer" is well defined and folds like our per-node Welford/P² stats. **The break:** per-frame **scope *counts* vary**. Our model assumes **one settle per node per run** — a bijection between nodes and per-run instances. An Insights capture has no such guarantee: a timer can fire 0, 1, or *N* times per frame (loops, conditional systems, `parallel_for`-style fan-out each emitting the same scope). So "aggregate timer X across frames" is not 1:1; you must choose a policy:
- **per-frame reduction first** (sum inclusive time of all instances of X in the frame, as the Timers tab's inclusive definition does), *then* fold across frames — closest to our per-node bar; loses within-frame multiplicity; or
- **per-instance** fold — matches the Timers tab's flat stats but is not "a node in an average frame".

Neither reproduces our clean node↔instance mapping. This is the **central impedance mismatch**: our identity is a *declared graph node*; Insights' identity is a *reused timer name*. **INFERENCE:** a UE capture instrumented specifically to mimic our model (one uniquely-named scope per logical node, fired exactly once per frame) would fold almost perfectly; an arbitrary capture will not.

### 4.3 Dependency structure / critical path — **only with the `task` channel**
This is the decisive one.
- If **`-trace=...,task`** is captured, `TaskTrace` gives real edges: `Launched` (node identity + debug name + target thread), `SubsequentAdded(A→B)` (the DAG edge), and `Started/Finished/Completed` timestamps. From these you can build a genuine dependency graph and run **our** backward-walk critical-path/criticality-frequency analysis. Insights already proves this data supports a critical path (Tasks Insights computes one). This is the **cleanest fit**, because UE's task graph *is* the same kind of object as our `Static_task_graph` — nodes with prerequisites/subsequents and per-node start/finish.
- If **only `cpu`/`frame`** is captured, you have **scope *nesting*, not a DAG**. Nesting encodes parent⊃child containment on one thread, not cross-thread happens-before. "Critical path" is **not well-defined** from wall-clock nested scopes alone — you cannot tell whether thread B's gap is a dependency wait on thread A or just idleness. You could approximate a critical *path through the frame* by wall-clock (the chain of longest-pole scopes bounding makespan), but it is a heuristic, not our edge-derived chain. **Our whole critical-path model presumes explicit edges; without the task channel it does not port.**

**Task-id vs our node identity mismatch:** UE task ids are **per-instance** (`GenerateTaskId()` mints a fresh id per launch), not stable across frames. To aggregate "the same node over 100 frames" you must **re-derive stable identity** from the task's `DebugName` (or the top CPU scope inside its Started→Finished window). Debug names are often non-unique or absent in shipping-ish captures → aggregation becomes name-clustering, fuzzier than our declared, stable node ids. And task tracing is **compiled out in Shipping** (`UE_TASK_TRACE_ENABLED` requires `!UE_BUILD_SHIPPING`, per `TaskTrace.h`), so the DAG is a development-build artifact.

### 4.4 Concurrency / utilization — **transfers**
Per-thread busy time (Σ scope inclusive durations, or top-level scope coverage) ÷ (thread count × frame wall) gives core utilization, and gaps give dead time — the same quantities our `Bucket_row`/busy-counter machinery computes, just reconstructed from the timeline instead of measured in-process. **Caveat:** "busy" from scopes undercounts un-instrumented work (anything without a `TRACE_CPUPROFILER_EVENT_SCOPE` is invisible), whereas our in-process busy counters see every task; and it *over*counts if scopes overlap by instrumentation error. Our own note that inline non-worker work is uncounted applies here too.

### 4.5 Async / overlap / GPU — **partially**
GPU work is on its own track with its own clock domain (queue timestamps); folding GPU into a CPU average-frame needs cross-domain alignment Insights does internally but a parser must redo. Async/latent work (loads, the render-thread-behind pattern) crosses frame boundaries, breaking the "everything in this node belongs to this frame" assumption our layout makes — you'd need the task/region channel to attribute it.

### 4.6 Where our assumptions break — summary
1. **fixed graph** → captures have a *variable* per-frame scope population; no build-once structure.
2. **one settle per node per run** → timers fire *N* per frame; task ids are per-instance.
3. **explicit edges** → present *only* with the task channel; otherwise nesting ≠ DAG and critical path is undefined.
4. **stable node identity** → must be re-derived from names, which are non-unique/absent in some builds and gone in Shipping.

**Bottom line for §4:** transfer is **excellent for the subset of captures that carry the task channel from a non-shipping build** (that data mirrors our model), **weak-to-heuristic for generic cpu/frame captures**. Frame segmentation and utilization port regardless; identity and critical path are the hard parts.

---

## 5. Work estimate: a `.utrace` parser producing similar results

Two fundamentally different build strategies; I cost the **from-scratch decoder** first (the literal question), then the **cheaper alternatives** (§5.6), which I recommend.

### 5.1 What already exists (so you don't reimplement it)

- **Headless CSV/TSV exporters** ship in `TraceInsights` — `Engine/Source/Developer/TraceInsights/Private/Insights/TimingProfiler/ViewModels/TimingExporter.h` exposes: `ExportThreadsAsText`, `ExportTimersAsText`, **`ExportTimingEventsAsText`** (raw per-instance rows: start, end, depth, timer, thread — filterable by time interval and named **region**), **`ExportTimerStatisticsAsText`** (min/max/avg inclusive+exclusive, sortable), `ExportTimerCalleesAsText`, `ExportCountersAsText`, `ExportCounterAsText`. Driven from the command line: `UnrealInsights.exe -OpenTraceFile=path.utrace -NoUI -AutoQuit -ExecOnAnalysisCompleteCmd="TimingInsights.ExportTimingEvents out.csv ..."` (also `TimingInsights.ExportTimerStatistics`, `TimingInsights.ExportCounterValues`; response files via `@=file.rsp`). There is even an AutomationTool wrapper `Engine/Source/Programs/AutomationTool/Scripts/ExportTimerStatisticsFromUtrace.cs`. **This is the single most important finding for cost:** the raw timeline is already extractable to CSV without touching the wire format.
  ([Automated Trace Analysis and CSV export](https://dev.epicgames.com/community/learning/knowledge-base/lwx6/unreal-engine-unreal-insights-automated-trace-analysis-and-csv-export), [forum: CLI export](https://forums.unrealengine.com/t/unreal-insights-cli-how-to-export-counter-values-to-csv/2661731))
- **`TraceServices` provider API** — a UE-linked C++ tool can `Analyze()` a `.utrace` and query `ITimingProfilerProvider`, `IFrameProvider`, `ITasksProvider`, `ICounterProvider` directly (full fidelity, incl. the task DAG the exporters don't emit).

### 5.2 Component decomposition and effort (from-scratch decoder path)

| # | Component | Scope | Est. |
|---|---|---|---|
| a | **Wire-format decoder** | Stream header + version dispatch; TID-packet demux; LZ4 decompress; per-thread stream reassembly; 7-bit/zig-zag ints; self-describing `NewEvent` schema table; aux-data strings; `EnterScope*`/`LeaveScope*` incl. `_TA`/`_TB` timestamp bases; sync serials | **4–7 pw** |
| b | **Event extraction** | Map decoded events → CPU scopes, frame begin/end, timer name table, counters, **task events** (`Launched`/`SubsequentAdded`/`Started`/`Finished`/`Completed`) | **1.5–2.5 pw** |
| c | **Per-frame scope model + cross-frame aggregation** | Frame segmentation; per-frame scope reduction; cross-frame Welford/P²/min-max; identity policy (§4.2/4.3) | **1.5–2.5 pw** |
| d | **Critical path + dead time + utilization** | Build DAG from task edges; backward-walk criticality frequency; per-thread busy→utilization; dead time. *Needs the task channel.* | **1.5–3 pw** |
| e | **Reuse our SVG renderer** | Adapt `graph_trace.h`/`dot_writer.h` layout to ingest the reconstructed model instead of live counters | **1–2 pw** |

**From-scratch total: ~10–17 person-weeks**, dominated by (a). Add **+2–4 pw** ongoing per major UE version for format churn (§2.4).

### 5.3 Risk areas
1. **Format churn** — protocol 5→6→7 already moved timestamp encoding; a hand decoder is a permanent maintenance tax pinned to specific `Protocol*`/`ETransport` versions. **High.**
2. **Task-graph availability** — the DAG (hence critical path) exists only when `-trace=task` is captured *and* the build is non-shipping (`UE_TASK_TRACE_ENABLED`). If your captures are cpu/frame only, §5.2(d) degrades to a wall-clock heuristic. **High for generic captures, low if you control capture.**
3. **Standalone `TraceAnalysis` viability** — `TraceAnalysis`/`TraceServices` are engine modules with deep `Core` dependencies (`TArray`, `FString`, memory, platform). Compiling them **outside** a UE build is impractical; the realistic "reuse" is a **UE-linked** program (a small `Program` target or commandlet), not a detached library. **Medium.**
4. **Identity re-derivation** — clustering per-instance task ids / reused timer names into stable "nodes" is heuristic and capture-dependent (§4.3). **Medium.**

### 5.4 Easier alternatives (recommended over 5.2)
- **A. Drive `TraceServices` in a small UE-linked tool.** A custom `Program` target links `TraceAnalysis`+`TraceServices`, calls `FTraceAnalysisModule`/`Analyze`, and queries the providers — you get CPU scopes, frames, counters, **and the task DAG** at full fidelity, then hand the model to our SVG renderer. **Skips §5.2(a)(b) entirely.** Est. **3–5 pw**. Cost: requires a UE source build/SDK to compile/link.
- **B. Use the headless CSV exporters (§5.1) + a thin ingester.** No engine linkage at all: run `UnrealInsights.exe -NoUI` to emit `ExportTimingEvents` (raw per-instance rows) + `ExportTimerStatistics`, parse the CSV, do our cross-frame fold + SVG. Est. **2–3 pw**. **Limitation:** the built-in exporters emit **timing events/timers/counters, not the task DAG** (no `ExportTasks`), so you get frames/utilization/per-timer stats but **not an edge-derived critical path** — you'd approximate it from nesting/wall-clock. Best MVP if you don't need the true critical path.
- **C. Different interchange entirely — Chrome Trace JSON.** UE can also emit chrome-tracing-style JSON in places, and many tools speak it; but it is *also* nesting-only (no DAG) and lossy vs `.utrace`. Only attractive if you already have a JSON pipeline. Est. **1–2 pw**, weakest fidelity.

### 5.5 Net recommendation on cost
The from-scratch decoder (§5.2) is **not worth it** unless a standalone, engine-independent binary is a hard requirement. **Alternative A** (TraceServices API) is the right tool if you need the task DAG/critical path; **Alternative B** (CSV export) is the fastest path to frames+utilization+per-timer stats.

---

## 6. Bottom line

1. **Is a `.utrace` → our-visualization pipeline worth building?** Only conditionally. Our tool's distinctive value — *average-frame with per-node variance, criticality frequency, packed concurrency layout* — is genuinely absent from Insights (which only does flat range aggregation, §3.2). So there *is* a gap worth filling. But the value materializes **only when the source data carries a task DAG**; on generic cpu/frame captures you can reproduce frames + utilization + per-timer stats but **not** the edge-derived critical path that is our headline (§4.3). So: worth it **iff** your captures include the `task` channel from non-shipping builds, or you control instrumentation to make scope↔node a clean 1:1.

2. **Minimum viable version.** Alternative **B** (§5.4): `UnrealInsights.exe -NoUI -AutoQuit -ExecOnAnalysisCompleteCmd="TimingInsights.ExportTimingEvents ..."` → parse CSV → fold Game frames → feed `graph_trace.h`'s renderer with per-timer bars + utilization + dead time. **~2–3 pw**, no engine linkage, no wire decoder. Ships the average-frame *layout* and the utilization/dead-time headlines; omits the true critical path.

3. **Recommended path.** If the critical path matters (it is the reason our tool exists), go **Alternative A**: a small UE-linked program over **`TraceServices`** providers (`ITimingProfilerProvider` + `IFrameProvider` + **`ITasksProvider`**), reconstruct the DAG from `SubsequentAdded` edges + `Started/Finished` times, run our existing backward-walk criticality analysis, render with our SVG. **~3–5 pw**, needs a UE SDK to build. Avoid the from-scratch decoder (§5.2, ~10–17 pw + perpetual churn tax) unless engine-independence is mandatory.

4. **The cleanest conceptual fit** — worth stating because it validates the whole exercise — is that **UE's Tasks trace *is* the same object as this library's model**: named nodes, prerequisites/subsequents (`SubsequentAdded` = our derived edge), per-node start/finish, and even a notion of critical path Insights already computes. A `TasksProvider` → our-renderer bridge is essentially transcoding one task-DAG-with-timings into another, which is why that subset ports almost losslessly while arbitrary CPU-scope captures do not. If a pipeline is built, **anchor it on the task channel**, not on raw CPU scopes.

---

### Source index
Source files (EpicGames/UnrealEngine `release`): `Runtime/TraceLog/Public/Trace/Detail/Protocol.h` + `Protocols/Protocol{0,5,7}.h`; `Runtime/TraceLog/Public/Trace/Detail/Transport.h`; `Runtime/TraceLog/Private/Trace/Writer.cpp`; `Developer/TraceAnalysis/Private/Analysis/Transport/TidPacketTransport.{h,cpp}` + `StreamReader.h`; `Runtime/Core/Public/Async/TaskTrace.h`; `Runtime/Core/Public/ProfilingDebugging/CpuProfilerTrace.h`; `Runtime/Core/Public/ProfilingDebugging/MiscTrace.h`; `Developer/TraceInsights/Private/Insights/TimingProfiler/ViewModels/TimingExporter.h`; `Programs/AutomationTool/Scripts/ExportTimerStatisticsFromUtrace.cs`.

Docs: [Developer Guide to Tracing](https://dev.epicgames.com/documentation/en-us/unreal-engine/developer-guide-to-tracing-in-unreal-engine) · [Trace in UE5](https://dev.epicgames.com/documentation/en-us/unreal-engine/trace-in-unreal-engine-5) · [Timing Insights](https://dev.epicgames.com/documentation/unreal-engine/timing-insights-in-unreal-engine-5) · [Timers and Counters tabs](https://dev.epicgames.com/documentation/unreal-engine/using-the-timers-and-counters-tabs-in-unreal-engine) · [Task Graph Insights](https://dev.epicgames.com/documentation/unreal-engine/task-graph-insights-in-unreal-engine-5) · [Automated Trace Analysis and CSV export](https://dev.epicgames.com/community/learning/knowledge-base/lwx6/unreal-engine-unreal-insights-automated-trace-analysis-and-csv-export) · [forum: CLI CSV export](https://forums.unrealengine.com/t/unreal-insights-cli-how-to-export-counter-values-to-csv/2661731)
