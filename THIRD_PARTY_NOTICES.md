# Third-party notices and license exceptions

QueueCache uses the [MIT License](LICENSE) except for the files listed below. **Each listed file, including the QueueCache modifications in that file, is distributed under the Microsoft Limited Public License (MS-LPL).** These are whole-file exceptions; the root MIT grant does not replace their terms. All other original QueueCache code and documentation is covered by MIT.

## Microsoft sample code — MS-LPL

The full license is included in [LICENSES/MS-LPL.txt](LICENSES/MS-LPL.txt). Preserve the copyright notices, this attribution, and that license when redistributing the covered source. MS-LPL includes a requirement to retain its source-distribution terms and a limitation to software running on Microsoft Windows.

| File | Origin | Upstream copyright notice |
| --- | --- | --- |
| [qcache/mainwdm.cpp](qcache/mainwdm.cpp) | Adapted DiskPerf lifecycle, attachment, and request-forwarding code. | Copyright (C) Microsoft Corporation, 1991 - 1999 |
| [qcache/qcache.h](qcache/qcache.h) | Mixed QueueCache declarations and device-extension declarations/comments corresponding to DiskPerf. | Copyright (C) Microsoft Corporation, 1991 - 1999 |
| [scsichk/scsichk.c](scsichk/scsichk.c) | Modified DiskPerf driver. | Copyright (C) Microsoft Corporation, 1991 - 1999 |
| [scsichk/scsichk.inf](scsichk/scsichk.inf) | Modified DiskPerf installation file. | Copyright (c) Microsoft Corporation |
| [scsichk/scsichk.rc](scsichk/scsichk.rc) | Modified DiskPerf resource file. | Copyright (C) Microsoft Corporation, 1992 - 1999 |
| [scsilog/debug.cpp](scsilog/debug.cpp) | Adapted CLASSPNP SCSI/SRB/sense diagnostic string helpers. | Copyright (C) Microsoft Corporation, 1991 - 2010 |

An entry applies whenever that file is present in the checked-out revision. Whole-file exceptions avoid an uncertain line-by-line licensing split in mixed source files. They do not attribute the independently developed QueueCache cache implementation to Microsoft.

The licensing basis is the specific **MS-LPL** declaration in the official archived [WDK 8.0 sample-package README](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/README.md), which contains the compared [DiskPerf source](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/%5BC%2B%2B%5D-Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/C%2B%2B/WDK%208.0%20Samples/DiskPerf%20Storage%20Filter%20Driver/Solution/src/diskperf.c), its [INF](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/%5BC%2B%2B%5D-Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/C%2B%2B/WDK%208.0%20Samples/DiskPerf%20Storage%20Filter%20Driver/Solution/src/diskperf.inf) and [resource](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/%5BC%2B%2B%5D-Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/C%2B%2B/WDK%208.0%20Samples/DiskPerf%20Storage%20Filter%20Driver/Solution/src/diskperf.rc), and the [CLASSPNP debug source](https://github.com/microsoftarchive/msdn-code-gallery-microsoft/blob/21cb9b6bc0da3b234c5854ecac449cb3bd261f29/Official%20Windows%20Driver%20Kit%20Sample/Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/%5BC%2B%2B%5D-Windows%20Driver%20Kit%20%28WDK%29%208.0%20Samples/C%2B%2B/WDK%208.0%20Samples/ClassPnP%20Storage%20Class%20Driver%20Library/Solution/src/debug.c). This distribution follows those specific terms. It does not rely on the surrounding archive's generic MIT file as permission to relicense the sample code.

## LoopBack acknowledgment

The original acknowledgment of Mayur Thigale's LoopBack Filter Driver remains in `qcache/qcache.h`. The maintainer identifies it as early boilerplate/inspiration. Comparison with a preserved copy found ordinary driver API patterns and substantially different QueueCache implementations, including independently developed caching behavior. This acknowledgment is retained without asserting that the LoopBack example itself has been relicensed under MIT or MS-LPL.

## History and external dependencies

The project licensing, file exceptions, and missing attribution headers were applied during publication preparation on 2026-09-07, including to retained historical snapshots. Original commit dates describe the original development, not the date of this licensing cleanup. The grants and exceptions above also apply to those historical copies of this repository.

External helper libraries, SDK/WDK headers, Visual Studio components, and binaries referenced by the build are separate dependencies governed by their own terms. They are not included in the root MIT grant merely because a project references them.
