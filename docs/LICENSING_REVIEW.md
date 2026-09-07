# Licensing and provenance

The maintainer selected **MIT with explicit Microsoft sample exceptions** on 2026-09-07. The applicable grants are in [LICENSE](../LICENSE), [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), and [LICENSES/MS-LPL.txt](../LICENSES/MS-LPL.txt).

The six files listed in the notices are distributed in their entirety under MS-LPL, including their QueueCache modifications. All other original QueueCache code and documentation uses MIT. Existing Microsoft notices are retained, and the missing notices have been restored in the cache driver's lifecycle source/header and the CLASSPNP-derived log helper.

## Source comparison

The provenance review examined all five original source snapshots and the current implementations. It used manual inspection and comparisons with whitespace, comments, and selected renamed identifiers normalized. This is evidence of technical similarity and origin, not a numerical determination of copyright ownership.

| Repository material | Evidence |
| --- | --- |
| `scsichk/scsichk.c` | Extensive implementation matches to Microsoft's [DiskPerf sample](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/%5BC%2B%2B%5D-Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/C%2B%2B/WDK%208.0%20Samples/DiskPerf%20Storage%20Filter%20Driver/Solution/src/diskperf.c), including Plug and Play, synchronous forwarding, performance registration, and WMI. Large consecutive sections remain after the driver-prefix rename. |
| `scsichk/scsichk.inf` and `scsichk/scsichk.rc` | Modified DiskPerf installation and resource files with retained Microsoft notices. |
| `qcache/mainwdm.cpp` | Adapted DiskPerf lifecycle and forwarding material, including `QCacheDeviceUsageNotification`, `QCacheForwardIrpSynchronous`, `QCacheSyncFilterWithTarget`, and related routine comments. |
| `qcache/qcache.h` | Some device-extension declarations/comments correspond to DiskPerf; the file mixes these with QueueCache declarations and retains the separate LoopBack acknowledgment. The whole-file MS-LPL exception avoids claiming MIT for mixed material. |
| `scsilog/debug.cpp` | Extensive matches to Microsoft's [CLASSPNP debug source](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/%5BC%2B%2B%5D-Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/C%2B%2B/WDK%208.0%20Samples/ClassPnP%20Storage%20Class%20Driver%20Library/Solution/src/debug.c), including SCSI operation, sense, additional-sense, and SRB status string helpers. |

The specific archived [WDK 8.0 package declaration](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/README.md) identifies MS-LPL. The repository follows that declaration and includes the full license. The archive's generic root MIT file was not used to override the sample-specific terms. The separate current Windows-driver-samples repository uses MS-PL; that is not the licensing route selected for this historical material.

## LoopBack acknowledgment

The header credits Mayur Thigale's LoopBack Filter Driver. The maintainer recalls early boilerplate or inspiration rather than substantial continuation of the example.

Comparison with preserved [LbkFlt.c](https://github.com/tianye606/Sample/blob/7d72ab46dfe0bd31fd8ff057077ae934ea9b84b7/DriverSample/FromCodeProject/LoopbackFilter/LBKFLT/LbkFlt.c) and [LbkFlt.h](https://github.com/tianye606/Sample/blob/7d72ab46dfe0bd31fd8ff057077ae934ea9b84b7/DriverSample/FromCodeProject/LoopbackFilter/LBKFLT/LbkFlt.h) supports that account. The example attaches to a named loopback device, forwards requests, and demonstrates read completion. It has no delayed-write queue, cache admission policy, cached-read merge logic, or background write worker. No large retained LoopBack implementation was identified in the QueueCache snapshots examined.

The retrieved copy is a third-party mirror. The original [CodeProject article](http://www.codeproject.com/KB/system/loopback.aspx) could not be retrieved, so no MIT, MS-LPL, CPOL, or other license is inferred for the example itself. The original credit is preserved. An original download or license record, if recovered, would provide better provenance evidence.

## Scope of the publication cleanup

Missing headers and the complete licensing files were restored in retained historical snapshots as well as the latest tree. Runtime implementation was not changed. The original development dates have been retained, and the notices record that the licensing cleanup occurred on 2026-09-07.

MIT-only distribution would require separately resolving the Microsoft-derived files; that is not necessary for the selected MIT-with-exceptions arrangement. Any future replacement of driver implementations is separate work requiring appropriate validation. The [experimental status](../README.md) and [known storage-safety issues](KNOWN_ISSUES.md) remain applicable.
