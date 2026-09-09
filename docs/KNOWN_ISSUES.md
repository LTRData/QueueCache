# Known issues and contributor work

These findings describe the **historical engine** preserved under `driver/qcache/`, not the replacement opt-in `lab-writecache` engine. See the root README for the new engine's contracts and remaining validation limits. Moving source paths does not mean these legacy defects were repaired.

**QueueCache is experimental and can lose acknowledged writes or corrupt a filesystem.** This document is a starting point for investigation, not a claim that the driver is otherwise correct.

Review basis: the original source snapshots from 2015-2026, inspected on 2026-09-07. Runtime code is unchanged by the publication cleanup; source links account for restored attribution headers. The review covered the five reachable commits and their file versions. The findings below describe the current source. No driver build, installation, Driver Verifier run, or runtime reproduction was performed for this documentation pass.

“Confirmed in source” identifies a visible control-flow or accounting defect. “Design/validation gap” identifies behavior that requires a defined contract and runtime evidence. Validation steps are proposed work, not completed tests.

## QC-01 — Flush and write-through completion are not reliable durability barriers

**Confirmed in source; highest priority.**

[QCacheQueueIrp](../driver/qcache/queue.cpp#L14) admits both writes and `IRP_MJ_FLUSH_BUFFERS` to the lazy queue. [QCacheQueueLazyWriteIrp](../driver/qcache/queue.cpp#L127) completes the original request successfully after queue insertion. It does not exclude incoming writes marked `SL_WRITE_THROUGH`. The worker later sets write-through on its replacement write, which does not repair the original request's early completion.

The private `IOCTL_QCACHE_FLUSH` and `IOCTL_QCACHE_OFF` requests are completed as successful queue markers in [QCacheDispatchQueuedItem](../driver/qcache/queue.cpp#L269); they do not issue a lower-device flush. They also cannot report earlier background-write failures described in QC-02.

A filesystem or application can therefore observe success while its data is still volatile. Microsoft's [flush IRP contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mj-flush-buffers) requires transfer of cached data before completion; the [write-through flag](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_io_stack_location) expresses a request to reach persistent storage.

**Suggested work:** Define a serialized durability barrier, preserve incoming write semantics, wait for relevant writes and the lower flush, and propagate failures. Define exactly what the private control commands promise.

**Validate:** Delay lower writes and flushes independently. Verify that barrier completion cannot overtake either; inject write/flush failures and verify the caller sees failure. Exercise an incoming write-through request while ordinary writes are queued.

## QC-02 — Failed background writes can be discarded silently

**Confirmed in source; highest priority.**

[QCacheDispatchQueuedItem](../driver/qcache/queue.cpp#L290) builds a replacement request and waits if it is pending, but does not examine its final status or transferred byte count before removing the queue entry and freeing the cached buffer. `LastErrorCode` is updated when IRP construction fails, not when a lazy lower-device write or flush fails. The original caller has already received success.

This loses the dirty copy after a failed or short write and can make later statistics or control requests appear successful. The `IO_STATUS_BLOCK` supplied to the builder is also declared inside a block that ends before the subsequent call and wait; its lifetime needs to encompass completion. Microsoft documents that the [builder's status block receives the final lower-driver result](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iobuildsynchronousfsdrequest).

**Suggested work:** Keep completion state alive, inspect final status and byte count, and design an explicit failed-cache state and dirty-data policy. Decide how later writes, reads, flushes, shutdown, and operator diagnostics expose a deferred failure. A previously completed write cannot simply be completed again with a new status.

**Validate:** Inject immediate and pending failures, successful short writes, and lower flush failures. Verify that no dirty data disappears as if successfully committed and that later barriers cannot report an unqualified success.

## QC-03 — Shutdown, power, and removal need a complete lifecycle

**Existing partial handling; design/validation gap with visible admission races.**

[QCacheShutdown](../driver/qcache/write.cpp#L67) does exist: it clears `IsCached` and queues the shutdown IRP. [QCacheRemoveDevice](../driver/qcache/mainwdm.cpp#L1171) asks the worker to drain and exit, then sends a lower flush. These paths are not evidence of a reliable shutdown guarantee.

The source registers a shutdown dispatch function but makes no shutdown-notification registration call. Delivery through the supported attachment stacks therefore needs to be established; absence of local registration alone does not prove that an upper driver never forwards shutdown. Microsoft says [only one driver per device stack should register](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioregistershutdownnotification), so adding registration blindly is not a complete fix.

[Dispatch setup](../driver/qcache/mainwdm.cpp#L322) forwards power requests through the default handler. [Usage notifications](../driver/qcache/mainwdm.cpp#L1242) adjust paging-path counts and power flags without defining a cache policy for paging, hibernation, or dump devices.

Clearing `IsCached` does not prevent new queue entries: [QCacheQueueIrp](../driver/qcache/queue.cpp#L20) reads it before allocation and queue locking, and the non-lazy path still enqueues work. An already-admitted lazy write can be inserted after an off/shutdown marker. Removal stops the worker before [IoReleaseRemoveLockAndWait](../driver/qcache/mainwdm.cpp#L1228) closes admission through the remove lock, leaving an interval in which new work can be stranded.

**Suggested work:** Define admission, draining, failure, and stopped states; serialize state changes with enqueueing; establish shutdown delivery and supported power/PnP transitions; preserve lower-stack availability until required I/O completes. Account for the unbounded waits and allocation dependencies in QC-04.

**Validate:** Race writers and cache-control requests with shutdown/removal. Exercise restart, suspend/resume, hibernation, stop/restart, surprise removal, and lower-device errors on disposable storage. Do not infer crash or power-loss protection from a successful orderly-shutdown test: this implementation has no persistent recovery store.

## QC-04 — Queue limits do not establish bounded memory use or forward progress

**Confirmed mechanisms; resource policy remains incomplete.**

[Queue admission](../driver/qcache/queue.cpp#L24) checks the existing byte/item counters before reserving space, outside the queue lock. It does not include the incoming allocation size in the check. Large requests and concurrent writers can overshoot the configured limits. When the check rejects lazy completion, the fallback still allocates and queues a `WRITE_QUEUE_ITEM`; it does not impose a hard bound on all outstanding entries or their retained original IRPs/MDLs.

The defaults in [qcstats.h](../driver/qcache/qcstats.h#L39) are 10,000 items and 500 MiB per filtered device. Cached payloads use [nonpaged-pool allocation](../driver/qcache/wkmem.hpp#L3). These are configured thresholds, not proven safe system-wide budgets.

The worker allocates the lower IRP after acknowledging a cached write. If [IoBuildSynchronousFsdRequest fails](../driver/qcache/queue.cpp#L306), it leaves the dirty entry at the queue head and returns. The [worker loop](../driver/qcache/thread.cpp#L66) immediately retries it, with no backoff or reserved resources. Persistent allocation failure can spin without draining the queue; a lower request that never completes can also block the worker and shutdown indefinitely.

**Suggested work:** Reserve capacity atomically, include the full request cost, bound the fallback, consider aggregate usage across devices, and design an allocation-failure strategy that can still make progress. Define behavior for a stalled lower device without discarding acknowledged data.

**Validate:** Use concurrent writers, oversized requests, multiple filtered devices, and fault injection at every allocation site. Check actual pool usage, queue invariants, CPU use under sustained failure, and eventual drain after transient failures.

## QC-05 — Allocation and event failures leave broken cleanup paths

**Confirmed in source; useful bounded starting points.**

In [QCacheQueueLazyWriteIrp](../driver/qcache/queue.cpp#L147), a remove lock is acquired before mapping the source MDL and allocating the payload. If MDL mapping fails, the function returns without deleting the item or releasing that lock. If payload allocation fails, it deletes the item but still does not release the lock. The [item destructor](../driver/qcache/qcache.h#L182) frees only the buffer. The leaked lock can prevent device removal from completing.

[DriverEntry](../driver/qcache/mainwdm.cpp#L120) can continue after creating or referencing `QCacheLowMemCondition` fails. [Queue admission](../driver/qcache/queue.cpp#L33) then passes that potentially null pointer to `KeResetEvent` or `KeSetEvent`. Missing high-memory condition objects, by contrast, are treated as permission to cache.

**Suggested work:** Balance each successful acquisition on every failure exit. Establish whether essential event creation must fail initialization, and define a conservative policy when memory-condition information is unavailable.

**Validate:** Fail MDL mapping, payload allocation, event creation, and event referencing independently. Verify that each request completes exactly once, allocations and remove-lock counts balance, and cleanup finishes without accessing a null event.

## QC-06 — Mixed cache/disk reads report the wrong byte count

**Confirmed in source; useful bounded starting point.**

[QCacheRead](../driver/qcache/read.cpp#L114) copies overlapping dirty ranges directly into the caller's buffer, then uses `SCATTERED_IRP` to read uncovered ranges. [The scatter completion routine](../driver/qcache/partialirp.cpp#L164) adds only lower-read bytes to `BytesCompleted`; the [scatter destructor](../driver/qcache/qcache.h#L240) reports that value to the original caller. Cached bytes are never added to this completion count.

For example, an otherwise successful 1,024-byte read with 512 bytes satisfied from cache and 512 from disk reports 512 bytes, despite filling both ranges. Fully cached reads take a different completion path and do not demonstrate this defect.

**Suggested work:** Track covered byte ranges and account for cached plus lower-read bytes without double-counting overlapping writes. Define the treatment of short lower reads.

**Validate:** Check both data and `IoStatus.Information` for fully cached, uncached, and mixed reads, including overwritten cache ranges, several gaps, synchronous/asynchronous lower completions, and short or failed lower reads.

## QC-07 — Attachment and registry failure paths can use invalid state

**Confirmed in source.**

[QCacheAttachDevice](../driver/qcache/mainwdm.cpp#L732) returns success when a device is skipped as read-only or when device creation fails, without returning a valid extension. Other failure paths return success after deleting the extension. [QCacheAttachLegacyDevice](../driver/qcache/mainwdm.cpp#L451) treats success as proof that the output is valid and calls initialization with it. This can use an uninitialized or freed pointer.

The [AttachDevices registry parser](../driver/qcache/mainwdm.cpp#L378) also does not validate the value type or require termination within the supplied length. If `wcsnlen` reaches the end of an unterminated buffer, subtracting the string length plus a terminator from the unsigned remaining length can wrap and continue outside the buffer.

**Suggested work:** Return distinct skipped/failed/successful outcomes with valid output ownership; handle initialization failures and unwind already attached legacy devices. Validate registry types, lengths, and terminators before traversing the list.

**Validate:** Exercise read-only and unsupported targets, allocation/attachment failures, failure after one successful legacy attachment, and malformed/unterminated registry values. Check for leaked devices, workers, and references as well as invalid accesses.

## QC-08 — Other I/O contracts need targeted validation

**Design/validation gaps; these are not all demonstrated corruption bugs.**

- [Reads](../driver/qcache/read.cpp#L44) assume 512-byte granularity in alignment checks and bitmap ranges. [Writes](../driver/qcache/write.cpp#L25) do not perform the same alignment checks. Establish supported logical-sector sizes, check negative/overflowing offsets, and test sector-aligned splitting on 512-byte and 4 KiB devices.
- [Read cache lookup](../driver/qcache/read.cpp#L114) scans the whole write queue and copies data while holding a spin lock. Measure lock hold times and contention with large writes and long queues before treating the cache as a general performance improvement.
- [Control dispatch](../driver/qcache/ioctl.cpp#L65) decides whether most IOCTLs should be queued from their access bits. Access requirements do not define storage-ordering semantics. Audit TRIM/deallocation, pass-through commands, and other media-changing requests against dirty data.
- Queued original IRPs have no explicit cancellation handling. Establish ownership, cancellation, and remove-lock coverage for every dispatch path and ensure cancellation cannot race completion or teardown.
- [Statistics](../driver/qcache/ioctl.cpp#L35) are copied while writers update them through different synchronization paths. Treat them as approximate diagnostics until a coherent snapshot contract exists.

**Validate:** Use narrowly scoped tests for each contract, including cancellation/completion races, conflicting media operations, sector-size coverage, and Driver Verifier checks.

## QC-09 — Reproducible builds and a storage test harness are missing

**Confirmed repository gaps; no fresh build was attempted in this review.**

[qcache.vcxproj](../driver/qcache/qcache.vcxproj) and [qcachecmd.vcxproj](../qcachecmd/qcachecmd.vcxproj) use WDK 8.1 toolsets. The command project imports an absent `PropertySheet.props` and includes helper headers not supplied here. [scsilog.vcxproj](../legacy/scsilog/scsilog.vcxproj) uses v141 and references a sibling `LTRLib40.dll`; [scsichk.vcxproj](../legacy/scsichk/scsichk.vcxproj) uses the Windows 10 driver toolset. The only committed INF installs the separate SCSI logger.

[legacy/scsilog/debug.cpp](../legacy/scsilog/debug.cpp#L21) places the formatting-function definitions under `_DEBUG`, while the reader calls them unconditionally. That is a source-visible Release-link concern to verify when restoring the build.

**Suggested work:** Document or replace missing dependencies, establish an explicit supported build matrix, and provide a reproducible driver/control-tool build. Add an instrumented lower-device test harness with controllable failures and completion delays. Keep installation and recovery instructions restricted to disposable test systems.

**Validate:** Build from a fresh checkout without personal property sheets or sibling binaries. Then exercise the data-integrity, ordering, resource-failure, and lifecycle scenarios above. Build success alone is not a storage-safety result.

## QC-10 — The SCSI diagnostic tools have separate safety and privacy risks

**Confirmed source behavior; auxiliary tools, not cache protection.**

[ScsiChkIoCompletion](../legacy/scsichk/scsichk.c#L1584) allocates log records from nonpaged pool, relies on an assertion before dereferencing the allocation, and copies transfer payloads into records. Records accumulate in memory until [device removal writes them](../legacy/scsichk/scsichk.c#L925) to `\SystemRoot\scsichk.log`. There is no visible log-size budget. The on-disk format also serializes a native [LOGDATA structure](../legacy/scsichk/scsichk.h), including its `Next` pointer and architecture-dependent layout.

[The log reader](../legacy/scsilog/scsilog.cpp#L55) checks that a record header fits, but trusts `DataLength` when reading the payload and advancing. A truncated or malformed record can cause reads outside the mapped file.

**Suggested work:** Bound logging, handle allocation failure, establish a stable pointer-free file format, validate payload lengths, and make payload capture explicit. Use synthetic data in shared diagnostics.

**Validate:** Test allocation failure, high-volume logging, truncated records, oversized lengths, and the declared cross-architecture format. Inspect log output for actual transferred content and kernel addresses before sharing it.

## Suggested starting order

The durability, deferred-error, lifecycle, and memory-policy work (QC-01 through QC-04) determines whether the driver can ever be used safely beyond experiments. It needs an agreed contract before implementation.

Smaller contributions can start with failure cleanup (QC-05), mixed-read completion accounting (QC-06), or attachment/registry validation (QC-07), supported by reproducible builds and focused tests (QC-09). Keep changes bounded and state which safety problems remain. Fixing one item does not establish production readiness.
