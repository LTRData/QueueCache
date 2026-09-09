# QueueCache

## Managed tools and experimental desktop

The native driver is under `driver/qcache/`; historical utilities are under `legacy/`.
`src/QueueCache.Management` owns the driver protocol; `src/QueueCache.Operations` owns shared configuration and file workloads.
The CLI and Avalonia desktop call these libraries directly. The desktop never shells out to the CLI for cache control.

After installing on a disposable **secondary NTFS disk**, in elevated PowerShell:

```powershell
qcache apply Q: --accept-volatile-flush  # default: 4096 MiB, Fast preset, enabled
qcache apply Q: --preset Strict --budget-mib 256
qcache test Q: --report test-result.json
qcache benchmark Q: --size-mib 256 --passes 4 --report benchmark-result.json
qcache cache-status Q: --json
qcache watch Q:
qcache disable Q:
```

Fast is the user-facing default, **not silent permission to enable every disk**. It acknowledges eligible writes and application flushes in volatile RAM; loss/corruption after a crash remains possible. Apply requires explicit risk acceptance for Fast. Apply validates the target, drains/disables if needed, applies the budget/preset, enables, and verifies the result. It stops on failure without claiming an atomic rollback. Already matching configuration is a no-op. Configuration is not yet automatically restored after reboot.

`qcache test` is a file-only, current-boot suite: no formatting, raw writes, reboot, fault injection or policy changes. It checks unbuffered writes/overwrites/live reads, explicit drain/reopen, copy/rename hashes and settings/health. It deletes only its newly created discard probe, retaining other test files. TRIM observation can be skipped when Windows does not issue a notification during the observation window. Reports explicitly mark unsupported/skipped coverage; a pass is not 100% driver coverage. Run on an otherwise idle test disk, because global cache counters and manual draining also reflect other applications.

The built-in benchmark is a deterministic sequential file workload, **not a CrystalDiskMark score**: timing includes pattern generation and buffer copying; each overwrite pass is verified. A large unique-data working set measures different behaviour from repeatedly overwriting a small file. Final driver drain time is reported separately. Use matching policy, RAM budget and workload for comparisons. Cancellation retains files and leaves caching running; a lower-storage drain already in progress cannot safely be cancelled by discarding data.

Run `QueueCache.Desktop.exe` for the elevated Avalonia frontend: select a volume, Inspect, choose budget/preset, acknowledge Fast-mode risk, and Apply. It provides a live dirty bucket, status, flush/disable and shared test/benchmark actions. Closing the window does not disable caching. This initial UI has no saved configuration, historical graphs or clean-read bucket yet.

### Gathered writes and TRIM (0.3, experimental)

The new engine uses a pending FIFO plus free list, independent of payload addresses. A preallocated 256 KiB buffer gathers disk-adjacent blocks even when their RAM slots are scattered; the oldest pending version of each block is written first. There remains one outstanding background lower write. Payload, index, descriptors and gather buffer count toward the configured budget.

Validated aligned TRIM ranges pause new background submissions, wait for an already-issued write, and forward the original request. Only a successful lower TRIM permits retirement of matching pending blocks. Subsequent writes cannot be admitted by the serialized request worker until this operation completes. Unsupported forms retain the conservative drain/pass-through path. This is block-discard handling, not filename tracking.

Extended snapshots expose `DiscardedBytes`, `LowerWrites`, `BatchedWrites` and `TrimRequests`. The accounting identity is accepted = drained + dirty + coalesced + discarded. New clients check the legacy snapshot's capability flag before requesting the extended snapshot; querying old drivers does not probe unknown control codes that might force a drain.

### Build and installer

`build/Build.ps1 -LabWriteCache` builds the driver, both frontends and guarded test tools. GitHub Actions uses a separate build-number job and one shared four-part version across artifacts. Hosted jobs run only host-safe tests, never attach a filter to runner storage. Branch builds publish artifacts; successful master builds publish explicitly experimental prereleases.

`build/Sign-Lab.ps1` produces a test-signed lab package; `build/Build-Installer.ps1` compiles it using Inno Setup 6.7.1. The installer adds the controller to PATH, installs application files, runs guarded interactive driver setup and requests reboot. Existing attached-driver upgrades require detach/reboot followed by resuming setup and another reboot. Initial formatted-disk attachment requires explicit disk identity and confirmation. Nothing is formatted or automatically enabled. Removal drains/detaches before removing application tools; retained kernel service/binary are intentionally kept until safe post-reboot cleanup is implemented.

This is **not production signing**. Secure Boot/test-signing prerequisites remain explicit, private signing keys are never packaged, and installer/compiler redistribution/licensing must be reviewed before commercial distribution.

**Two separate implementations:** the historical `qcache.sys` engine and its known defects are preserved for investigation. The opt-in `-LabWriteCache` build produces `qcachelab.sys` from the new bounded write-cache engine, with separate control and validation code. Both remain experimental; the legacy warnings below are not a description of the new engine's intended flush contract.

## Experimental write-cache operator commands

After the guarded, test-signed lab installation and reboot, open an **elevated PowerShell** in the package's `controller` directory. Substitute the previously verified secondary disk number; never guess a target or use the OS disk.

```powershell
.\qcache.exe start PhysicalDrive1 4096
.\qcache.exe watch PhysicalDrive1
# Ctrl+C stops only the display. Caching continues.
.\qcache.exe flush PhysicalDrive1
.\qcache.exe disable PhysicalDrive1
```

`4096` means 4096 MiB (4 GiB), including cache metadata/slab overhead, not 4 GiB of usable payload. RAM is reserved up front; leave enough memory for Windows and applications. `start` configures then enables and intentionally fails if the cache is already enabled or dirty. Use `disable` and check success before changing the budget. Reboot returns to disabled/unallocated; configuration is not persisted.

The bucket shows pending dirty payload, including in-flight writes. **Accepted** means copied into RAM; **drained** means completed by the lower storage device, not necessarily persisted on physical media. In default strict mode, explicit flush/write-through requests are honored, so an application's use of them can reduce apparent caching gains. Full-cache throttling is expected. Any reported error needs investigation; do not discard dirty data by rebooting after a lower-I/O failure.

Use disposable files on the filtered disk. Compare the same workload/settings with caching off and on, and measure final flush time as well as write-return time. Small tests may mostly measure RAM/Windows caching rather than sustained disk throughput. No clean read-cache feature is implemented yet.

### Opt-in unsafe deferred flushes (0.2.3, experimental)

Strict policy remains the boot default. The experimental alternative deliberately acknowledges application/OS flush and eligible write-through requests while data can remain in volatile RAM. A reported successful save/flush can therefore be lost on failure, including filesystem metadata. This is not equivalent to durable storage or a promise of any benchmark score. Secondary-NTFS VM tests passed for deferred flush/write-through, rejection of an enabled policy change, explicit manual-flush failure/retry, and file hashes after a normal dirty-cache restart. These narrow tests do not establish production safety or sudden-loss durability.

With the matching new driver/controller installed, select it only on the disposable target, while disabled and clean:

```powershell
.\qcache.exe disable PhysicalDrive1
.\qcache.exe policy PhysicalDrive1 unsafe-defer --accept-volatile-flush
.\qcache.exe start PhysicalDrive1 4096
.\qcache.exe watch PhysicalDrive1
.\qcache.exe diagnostics PhysicalDrive1
```

`watch`/`cache-status` label this **UNSAFE-DEFER**. The selection survives configure/enable/disable in this boot, but not reboot. To restore strict policy, disable successfully, then use `policy PhysicalDrive1 strict` before enabling.

| Operation | Strict | Unsafe-defer |
| --- | --- | --- |
| Application/OS flush | Waits for admitted writes and lower flush | May return with dirty RAM while enabled; existing errors remain visible |
| Eligible write-through write | Bypasses RAM after prior drain | May be admitted to RAM |
| `qcache flush`, disable, retry | Real QueueCache drain/lower flush | Same real barrier; failures remain visible |
| Normal shutdown/device power-down/removal barriers | Drain | Still drain |
| Unknown controls, including TRIM | Preserve ordering | Still preserve ordering |

Manual commands drain data already admitted to QueueCache, not application buffers or dirty pages still held by Windows' file cache. Backpressure and the memory limit remain unchanged. Diagnostics distinguish application flushes, deferred flush/write-through requests, administrative and other barriers; `LastBarrierCode` helps investigate non-flush waits.

### Pending-block coalescing (0.2.4, experimental)

The fixed-budget index replaces the payload of an older pending write to the same aligned 4 KiB disk block. An in-flight buffer is immutable: a concurrent overwrite gets a newer pending entry, and completing the older write cannot remove that newer entry. Reads use the newest indexed contents. Payload, descriptors and index allocations all count toward the configured budget. `CoalescedBytes` reports accepted bytes superseded in RAM; they are not counted as disk-drained bytes. At a clean drain, accepted bytes equal drained plus coalesced bytes.

This first implementation caches only full aligned 4 KiB blocks. Sector-aligned partial-block writes drain prior data and pass through unchanged; they never fabricate a full block from partial contents. Unknown controls, including TRIM/discard, still drain before forwarding. The driver does not see filenames and does not yet drop cached blocks when a file is deleted. Do not interpret overwrite coalescing as deletion-aware caching or a retained clean read cache.

`qcache-file-tests ... --test-coalescing` uses a fresh 64 MiB file, writes it repeatedly, checks live unbuffered reads and the bounded dirty working set, then injects a lower-completion error, retries, drains and verifies the independent file hash. It requires strict initial policy and at least 128 MiB payload capacity; it selects unsafe mode for the experiment and restores strict mode and the original file oracle on success. Secondary-NTFS VM validation passed: 2 GiB of repeated writes peaked at about 64.2 MiB dirty, with newest-data reads and recovery hashes intact. Strict/unsafe flush regressions, concurrent capacity/wraparound, a partial-sector overwrite, and normal dirty-cache restart verification also passed. These are experimental lab results, not production certification or sudden-loss durability.

For a previously recorded disk that is now formatted, `Manage-Lab.ps1 -Action Upgrade` or `Reattach` additionally requires `-AllowFormattedDisk`. Identity, exact size, non-OS status and stopped-driver checks remain mandatory. Initial installation still requires an empty RAW secondary disk. These scripts never format a disk.

## Repository hygiene

Reusable code, test harnesses, `build/` and `lab/` scripts are source-controlled material. Personal credentials, notes, host keys and ad-hoc test scripts belong only in ignored `.lab/`. Generated outputs (`artifacts/`, `bin/`, `obj/`) and downloaded tool caches (`.packages/`, `.tools/`) are also ignored. Packaging explicitly selects deliverables; never archive the repository root or publish local lab directories. No private signing key belongs in a package.

### Write-cache source map

| Component | Responsibility |
| --- | --- |
| `driver/qcache/lab.cpp` | Exact-device attachment, request serialization, Windows device lifecycle |
| `driver/qcache/writecache.cpp` | Native bounded memory, admission, background drain, read coherence and barriers |
| `src/QueueCache.Management` | C# device/protocol access and UI-independent telemetry calculations |
| `src/QueueCache.Cli` | Operator commands and console presentation; no caching algorithm |
| `tests/` | Protocol regression checks and explicitly guarded disposable-disk workloads |
| `build/`, `lab/` | Reproducible packaging and explicit installation/recovery tooling |

Keep native request ownership and lifetime changes separate from presentation changes. Protocol changes require matching native/C# size, version and bounds tests. Driver load or successful compilation alone is not a correctness test. Future graphical controls should reuse the management library rather than introduce another driver protocol implementation.

Open `QueueCache.Managed.slnx` for C# development; `dotnet build QueueCache.Managed.slnx` builds the managed projects only. The historical `QueueCache.sln` is preserved. Use `build/Build.ps1 -LabWriteCache` for the explicit modern native/managed package rather than assuming the old solution selects the new driver.

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
| [qcache](driver/qcache) | The experimental write-cache filter driver. |
| [qcachecmd](legacy/qcachecmd) | Control/statistics utility with `stat`, `on`, `off`, and `flush` commands. The latter two are not a guarantee that data is durable. |
| [scsichk](legacy/scsichk) | A separate experimental SCSI diagnostic filter. It can record transferred data, including disk contents. |
| [scsilog](legacy/scsilog) | A utility for reading the diagnostic filter's binary log format. |

The `scsichk.inf` file installs the diagnostic filter; it is not an installer for QueueCache. Diagnostic logs can contain sensitive data and must be reviewed before sharing.

## Build and testing status

The new build entry point is `build/Build.ps1`. Historical projects and `QueueCache.sln` are retained for reference; the modern path builds `driver/qcache/QueueCache.Driver.vcxproj` and the C# controller, not the diagnostic filter or old controller with missing helper dependencies.

Prerequisites: Windows x64, PowerShell 7, .NET SDK from `global.json`, Visual Studio 2026 **Desktop development with C++** and the **Windows Driver Kit** individual component. The script restores matching SDK/WDK NuGet packages pinned in `build/packages.config`. See Microsoft's [WDK NuGet setup](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget). Install prerequisites using an elevated Visual Studio Installer; ordinary builds need no administrator access.

```powershell
./build/Build.ps1 -Configuration Release -Version 0.1.0.0
# Controller-only development does not need Visual Studio or the WDK:
./build/Build.ps1 -ManagedOnly
dotnet run --project tests/QueueCache.Management.Tests -c Release
```

Packages under `artifacts/packages` contain an explicitly staged controller, licenses, checksums and build metadata; full builds also include the **unsigned** SYS/PDB. This is not an installable driver package. No signing keys, installation, disk formatting, attachment or caching enablement is performed. Each run uses a fresh staging folder. Versions have four fields in 0..65535; CI uses `0.1.<run>.<attempt>` and builds x64 Debug/Release on a Windows VS2026 runner. The workflow requires its first successful hosted run before being considered validated.

Local x64 Debug and Release driver/controller builds and protocol/path regression checks pass with SDK/WDK NuGet 10.0.28000.2526. Native ABI assertions are compiled with the driver. Legacy allocation/operator-declaration warnings remain; compilation and protocol tests do not establish storage correctness. Debug configurations contain deliberate `KdBreakPoint()` checkpoints, including startup and shutdown paths; use a kernel debugger when investigating them.

## C# management milestone

`QueueCache.Management` owns device access and statistics decoding; `QueueCache.Cli` is the first frontend. Published Windows x64 builds are self-contained.

```powershell
qcache.exe list
qcache.exe status PhysicalDrive1 --json
qcache.exe watch D:
```

Listing finds Windows device names, not necessarily filtered devices. Status/watch require QueueCache already attached; an unfiltered device returns an error. The live bar displays legacy **queue memory including overhead**, not durable bytes or a separately retained read cache. Counters are approximate. Controller enable/configure/flush/disable commands are intentionally deferred until the driver has reliable contracts.

Next: repair initialization/attachment and build a reversible secondary-device lab installer; then fix bounded admission, failed writes, flush and shutdown before enabling write-back. Add precise dirty/in-flight/drained-byte telemetry alongside that work. A later Avalonia frontend can reuse the C# management library to show the cache bucket, rates and errors; the kernel driver must remain responsible for caching and draining even when no UI is running. Boot-disk and clean read-cache support remain later milestones.

## First-load lab harness (no caching)

`./build/Build.ps1 -LabPassThrough -Configuration Release -Version 0.1.2.0` builds **qcachelab.sys**, a separate minimal PnP pass-through harness in `driver/qcache/lab.cpp`. The legacy cache engine is excluded, not merely switched off. Ordinary reads, writes, flushes, power and shutdown requests go to the lower driver; private enable/off/flush commands return not-supported. Statistics show zero cache capacity and requested read/write byte counts. This tests deployment/control/lifecycle plumbing, not the cache algorithm or its known-issue fixes. No memory queue or worker exists in this build. The original `qcache.sys` remains experimental and must not be substituted into this installation procedure.

The installer uses a C# SetupAPI helper to register the filter on **one exact disk devnode**, never the disk class. The driver additionally checks that device's recorded driver key. Initial install refuses disk 0, boot/system disks, non-RAW/partitioned disks, unexpected sizes, existing per-device upper filters and an existing installation. It requires an explicit snapshot confirmation and active test-signing. All harness code is nonpageable; paging/hibernation/dump usage notifications requesting entry are rejected. These guards reduce risk but are not proof of correct kernel behavior.

To make a local test-signed package, pass the exact unsigned staging directory to `build/Sign-Lab.ps1 -UnsignedPackageDirectory <directory>`. It signs only Release lab builds, keeps a non-exportable private key in the current user's Windows certificate store, and packages only the public certificate. No system trust store is modified. CI does not sign or install drivers. Self-signed test drivers are not production-trusted.

On the disposable VM, extract that signed ZIP into a local folder. Run **64-bit elevated PowerShell** there:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\lab\Manage-Lab.ps1 -Action Preflight
# After securing BitLocker recovery information and disabling Secure Boot in VM firmware:
.\lab\Manage-Lab.ps1 -Action EnableTestSigning -SnapshotConfirmed
# Reboot, then reconfirm disk identity/size before the next command.
.\lab\Manage-Lab.ps1 -Action Install -DiskNumber <secondary-number> -ExpectedBytes <exact-bytes> -SnapshotConfirmed
# Reboot from the VM console, then:
.\lab\Manage-Lab.ps1 -Action Status -DiskNumber <secondary-number>
```

Secure Boot and test-signing changes are lab-only security reductions; do not delete firmware keys, EFI disks or TPM state. Preserve the whole-VM snapshot and recovery console. Do not disable Memory Integrity speculatively: test-signed binaries are required even with it enabled. See [Microsoft's test-signing prerequisites](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option). No command above automatically reboots, formats a disk or enables caching.

**Removal/recovery:** run `Manage-Lab.ps1 -Action Uninstall`, then reboot. It removes only `qcachelab` from the recorded devnode while preserving other filters. Service/binary and installation-state JSON are deliberately retained until unloading is verified; do not stop/delete the service while the filter is attached. If Windows or the disk fails to start, restore the whole-VM snapshot through the hypervisor console. Logs and target identity are under `%ProgramData%\QueueCacheLab`; review them before retrying a partial install. First load, restart, removal and data-integrity checks remain required runtime gates; preparation/build tests do not mark them passed.

### Serialized-worker and write-integrity lab gates

Add `-LabSerialized` to a `-LabPassThrough` build to queue disk reads/writes/flushes and control requests through a cancel-safe queue and a single passive-level worker. Outputs use the separate `lab-serialized` directory and package metadata records the option. This is still **no caching**: original requests complete only after the lower stack completes them. It isolates request lifetime/ordering plumbing before RAM write-back is integrated.

`tests/QueueCache.WriteTests` is an explicitly destructive C# integration tool, packaged under `write-tests`. It accepts an exact secondary disk number, byte size and PnP instance, then either `--write-disposable-region` or `--verify-only`. The write mode overwrites only the 64 MiB region starting at 1 GiB on an empty RAW secondary disk; it refuses disk 0, boot/system disks, identity/size mismatches, partitions and unsupported sector sizes. It uses aligned unbuffered I/O, deterministic overlapping patterns, exact transfer-count checks, flush and close/reopen verification. Verify-only regenerates the oracle without writing, including after a reboot. Successful execution is not proof of crash durability or cache correctness.

To update a lab binary, remove the filter registration and reboot first. `Manage-Lab.ps1 -Action Upgrade` uses the same disk/size/snapshot arguments and validates the new signed package, exact target and stopped service. It backs up the previous SYS, preserves the original recovery identity, copies and verifies the new SYS, then reattaches. Reboot again to load it. Never replace a running driver in place.

### Experimental bounded write-cache build

`./build/Build.ps1 -LabWriteCache -Configuration Release -Version 0.2.0.0` builds the new cache, not the legacy engine. Signing and installation additionally require explicit `-AllowWriteCache`. It starts disabled on the single recorded secondary disk. This build is **under runtime validation, not production-ready**.

```powershell
qcache configure PhysicalDrive1 64 # MiB, including preallocated slot metadata
qcache enable PhysicalDrive1
qcache watch PhysicalDrive1
qcache cache-status PhysicalDrive1 --json
qcache flush PhysicalDrive1
qcache disable PhysicalDrive1
```

The request worker admits ordinary writes to fixed nonpaged RAM storage; a separate thread drains it continuously. Full capacity applies backpressure. Dirty bytes include in-flight data until successful lower completion. A failed/short drain retains those records and stops automatic retries; `retry` explicitly retries and flushes, and can fail again. Reads overlay pending writes in acceptance order; fully covered reads can use RAM alone. Explicit flush/disable drains and sends a real lower flush. Write-through requests remain lower write-through requests, ordered after prior cached writes. Reconfiguration requires disabled, clean state; enable is not persisted across boots. Unexpected power/device loss can lose dirty RAM data.

The budget covers requested payload and descriptor allocations, not OS pool allocator internals, thread/device overhead or outstanding caller-owned IRPs. Records use 4 KiB slots, so sub-4 KiB writes can exhaust slots before filling the advertised payload bytes. `cache-status` distinguishes reserved memory, dirty payload, occupied slots and in-flight bytes. No clean read cache is retained. Raw controller pass-through and neither-I/O controls are rejected because their user pointers cannot safely be forwarded from the worker context; paging/hibernation/dump use remains unsupported.

`lab-delay` (0–2000 ms per drain operation) and `lab-fault` are diagnostic hooks in this **lab-only** build. Fault modes: 0 clears; 1/2 synthesize write failure/short-write status before submission; 3 fails the next flush; 4/5 alter a real lower request's completion status/byte count before the I/O manager reports it; 6 fails descriptor allocation; 7 fails the third payload slab after partial allocation. Completion injection does not undo bytes already written by the lower device. These are not normal cache settings or evidence of real hardware-fault coverage. Disable them after testing. `status` retains the compatibility statistics view; prefer `cache-status`/`watch` for the coherent new protocol.

Additional C# integration modes use the same exact-identity/RAW-secondary guard and fixed 64 MiB test region:

| Mode | Checks |
| --- | --- |
| `--write-and-read-disposable-region` | Immediate RAM reads, overlapping writes, full flush/reopen oracle and cache accounting |
| `--write-through-check` | Prior dirty writes drained before write-through; needs at least 2 MiB usable cache and a bounded lab delay |
| `--write-concurrent-check` / `--write-toggle-check` | Four writers/readers with concurrent flush or disable/enable cycles |
| `--write-cancellation-check` | Cancellation while queued behind flush and waiting for capacity; requires a small 2–8 MiB usable cache |
| `--write-performance-check` | Alternating off/on, warm-up plus three measured pairs; 2 MiB burst and 64 MiB workload, separate write-return/flush timing, byte verification; requires 4 MiB budget |
| `--write-dirty-prefix <bytes> <seed>` / `--verify-dirty-prefix <bytes> <seed>` | Establish novel dirty data without explicit flush, then read-only verification after operator-controlled reboot |

Ordinary write/verification modes accept trailing `--seed <nonzero-ulong>`; use the same seed for later verification. Critical tests reject an oracle already present on disk so an old successful write cannot hide a lost new one. `lab/Test-CacheFaults.ps1` orchestrates synthetic retention/retry tests. Capture native tester output explicitly (for example, `2>&1 | Tee-Object <log>`); an SSH PowerShell transcript alone may omit it. None of these tests validates OS disks, clean read caching, real hardware failures, or every power transition.

`tests/QueueCache.FileTests` is a separate NTFS harness, packaged as `file-tests` in new lab builds. Its first cache-enabled file workload and post-reboot hash verification passed on the disposable lab disk; broader filesystem stress remains unvalidated. It never formats or partitions a disk. A pre-prepared NTFS volume must map wholly to the exact non-OS secondary disk (both partition inspection and opened-volume extents are checked). `qcache-file-tests <letter> <disk> <exact bytes> <PnP instance> --write-new-files` creates a uniquely named directory containing two 64 MiB files and a seeded SHA-256 manifest; it tests copy/rename/partial overwrite, flushes the filesystem volume and verifies cache admission and final data. It does not replace or delete pre-existing user files. After reboot, use the same arguments with `--verify-files <run GUID>` for read-only verification. Test directories are deliberately retained for review; remove only an explicitly verified test directory after it is no longer needed.

File-test variants accept the same target identity arguments:

- `--write-new-files [MiB]`: 64..8192 MiB per file; reports source write-return and file-flush timing separately.
- `--baseline [MiB]`: the same workload, but requires the cache disabled. Keep the same RAM reservation when comparing on/off results.
- `--concurrent [MiB]`: two independent writers, with flush/read verification on the second thread; both files receive final hash verification.
- `--dirty-reboot`: creates and verifies fresh files, persists an independently computed expected manifest, then makes a fresh unbuffered 2 MiB prefix overwrite with a synthetic drain delay. Reports `READY_FOR_NORMAL_REBOOT` only if dirty payload remains. It does not restart automatically. Restart normally and use `--verify-files <run GUID>`; preparation alone is not a persistence PASS. If not rebooting, clear `lab-delay` and explicitly flush; do not leave diagnostic delays enabled for normal use.
- `--test-flush-policy`: start strict; prepare durable test metadata, opt into unsafe policy, verify dirty payload survives write-through plus application flush, check policy-change rejection and manual-flush error/retry, restore strict and verify the changed file. Passed on the secondary-NTFS test VM.
- `--dirty-reboot-unsafe`: prepare in strict mode, then opt into unsafe mode before the dirty write-through/application-flush sequence. Requires normal reboot and subsequent hash verification; passed on the secondary-NTFS test VM.

Unbuffered test writes use aligned allocations following Microsoft's [file-buffering requirements](https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering). They target only the newly created test file, never a raw disk range.

## Contributing

The repeatable read-only harness is `tests/QueueCache.LabTests`; lab builds package it under `tests`. Run it through `lab/Test-LabReads.ps1 -PackageDirectory <package> -TestExecutable <qcache-lab-tests.exe> -Mode attached` (or `detached` after removal/reboot). It uses the actual `qcache.exe`, validates statistics, compares eight 64 KiB reads, and checks that disk 0 rejects cache statistics. It never opens a write handle. It is a smoke test, not write/flush durability validation.

After uninstall/reboot and a successful detached test, use `Manage-Lab.ps1 -Action Reattach` with the same disk/size/snapshot arguments to reuse the recorded identity and verified stopped service/binary. The updated script accepts `-PackageDirectory` when staged separately from the immutable signed package. Reattachment still requires another reboot; do not use it before removal has completed. Runtime removal/reload validation must be recorded separately from registration success.

Start with a bounded item in [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md). Include the affected function, a reproducible scenario using disposable data, the expected behavior, and a test that demonstrates the change. Storage-ordering and lifecycle changes need an explicit description of when an operation may report success and what happens if the lower device fails.

Keep experimental-status warnings in place as individual issues are fixed. Use synthetic test data when possible, and do not commit captured disk contents, diagnostic logs, memory dumps, credentials, or signing keys.

## Licensing and attribution

QueueCache uses the [MIT License](LICENSE), with explicit **MS-LPL exceptions** for six files containing Microsoft DiskPerf or CLASSPNP sample material. Each exception applies to the entire listed file, including its QueueCache modifications. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the file list and required notices, and [LICENSES/MS-LPL.txt](LICENSES/MS-LPL.txt) for the full exception license.

The original credit to Mayur Thigale's LoopBack Filter Driver is retained as an acknowledgment of early boilerplate/inspiration. The experimental status and data-loss warnings remain applicable regardless of the license.

See [Licensing and provenance](docs/LICENSING_REVIEW.md) for source comparisons and the basis for the exceptions.
