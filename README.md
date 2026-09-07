# QueueCache

> **Experimental Windows storage filter driver — risk of data loss and filesystem corruption.**
>
> QueueCache keeps pending writes in volatile kernel memory and can acknowledge writes and flushes before the underlying storage completes them. A successful write, flush, or cache-off response is **not a reliable durability guarantee** in this implementation. Normal shutdown or restart, low-memory conditions, failed disk I/O, crashes, and power loss can result in lost data or a damaged filesystem.
>
> Use only for development with disposable secondary disks in an isolated test machine or VM. Keep the operating system, paging, hibernation, crash-dump storage, and valuable data off filtered devices. Do not deploy this driver in production.

QueueCache is an old experiment in using a memory-backed write queue to improve the apparent responsiveness of slow Windows storage volumes. It copies eligible write data into nonpaged pool, completes the original request, and sends the queued operation to the lower storage driver from a worker thread. Reads consult pending cached writes before reading the remaining ranges from disk.

The source is provided for investigation and further development. It has no validated production configuration, supported installation procedure, or demonstrated protection against data loss. The existing shutdown handler, queue limits, memory-condition checks, and cache control commands are incomplete safeguards.

See [Known issues and contributor work](docs/KNOWN_ISSUES.md) for source-based findings, suggested starting points, and validation criteria. The highest priorities are:

- Correct flush and write-through completion semantics.
- Handling and reporting failed background writes without silently discarding dirty data.
- Reliable shutdown, power-transition, and device-removal behavior.
- Bounded memory use and forward progress under allocation failure.
- Correct partial-read completion and initialization failure handling.

## Repository contents

| Path | Purpose |
| --- | --- |
| [qcache](qcache) | The experimental write-cache filter driver. |
| [qcachecmd](qcachecmd) | Control/statistics utility with `stat`, `on`, `off`, and `flush` commands. The latter two are not a guarantee that data is durable. |
| [scsichk](scsichk) | A separate experimental SCSI diagnostic filter. It can record transferred data, including disk contents. |
| [scsilog](scsilog) | A utility for reading the diagnostic filter's binary log format. |

The `scsichk.inf` file installs the diagnostic filter; it is not an installer for QueueCache. Diagnostic logs can contain sensitive data and must be reviewed before sharing.

## Build and testing status

The projects retain historical Visual Studio/WDK configurations. QueueCache and its control utility reference WDK 8.1 toolsets; the diagnostic projects use different toolsets. The control utility also references an absent `PropertySheet.props` and helper headers, and the log reader references an external `LTRLib40.dll`. A fresh checkout is not a self-contained, verified build.

No automated tests or CI workflow were present at the source revision reviewed for these notes. Successful compilation would not establish storage correctness. Debug configurations also contain deliberate `KdBreakPoint()` checkpoints, including startup and shutdown paths; use a kernel debugger when investigating them.

## Contributing

Start with a bounded item in [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md). Include the affected function, a reproducible scenario using disposable data, the expected behavior, and a test that demonstrates the change. Storage-ordering and lifecycle changes need an explicit description of when an operation may report success and what happens if the lower device fails.

Keep experimental-status warnings in place as individual issues are fixed. Use synthetic test data when possible, and do not commit captured disk contents, diagnostic logs, memory dumps, credentials, or signing keys.

## Licensing and attribution

QueueCache uses the [MIT License](LICENSE), with explicit **MS-LPL exceptions** for six files containing Microsoft DiskPerf or CLASSPNP sample material. Each exception applies to the entire listed file, including its QueueCache modifications. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the file list and required notices, and [LICENSES/MS-LPL.txt](LICENSES/MS-LPL.txt) for the full exception license.

The original credit to Mayur Thigale's LoopBack Filter Driver is retained as an acknowledgment of early boilerplate/inspiration. The experimental status and data-loss warnings remain applicable regardless of the license.
