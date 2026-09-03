# Third-party notices and LGPL compliance

LuxTrace itself is Copyright (C) 2026 Kutay Gunal, available under the
[GNU AGPL v3 only](LICENSE) or under a [commercial licence](LICENSING.md). This
file covers the two libraries it links but does **not** redistribute, and the
obligations those libraries place on whoever ships a built LuxTrace — under
either arm of the dual licence.

| Component | Version | Licence | Linking |
|---|---|---|---|
| OpenCASCADE Technology | 7.8.1 (vcpkg) | LGPL-2.1, with OCCT's additional exception | Dynamic, against the vcpkg `bin/*.dll` |
| Qt | 6.8.2 (MSVC 2022 kit) | LGPL-3.0 | Dynamic, via `windeployqt` |

Neither library's source is vendored in this repository; the build resolves OCCT
from vcpkg and Qt from a kit you install yourself. Nothing here derives from
either beyond their published headers.

## Licence compatibility

**Qt (LGPL-3.0) with AGPL-3.0 — clean.** LGPL-3.0 is GPL-3.0 plus additional
permissions, and its section 4 expressly contemplates a combined work. AGPL-3.0
is GPL-3.0 compatible in this direction. The combined work is distributable
under the AGPL, which is exactly what the open arm does.

**OpenCASCADE (LGPL-2.1) with AGPL-3.0 — clean, by either of two routes.** The
route that applies here is LGPL-2.1 section 6: LuxTrace is a *work that uses the
Library*, linked dynamically, with no part of OCCT merged into `LuxTrace.exe`,
so the combined work may be distributed under terms of the author's choosing
provided the relink conditions below are met. Independently, LGPL-2.1 section 3
permits treating a copy of the library as GPL-2.0-or-later, and the "or later"
reaches GPL-3.0, which AGPL-3.0 is compatible with. OCCT's additional exception
addresses the linking question a third time. Keep the link dynamic and no part
of this is close.

**Neither library imposes copyleft on LuxTrace.** The reciprocity in this
project is the AGPL's, chosen deliberately, not something the LGPL forced.

**The commercial arm changes nothing here.** A commercial licensee is released
from the AGPL's obligations by the copyright holder, who has no power to release
anyone from Qt's or OCCT's. The obligations in the next section travel with
every binary, sold or free.

## If you distribute a built LuxTrace

These are the distributor's obligations, not this repository's. Meet all four.

1. **Ship the libraries as their own DLLs.** Never statically linked, never
   merged into `LuxTrace.exe`. This is the condition the whole analysis above
   rests on.

2. **Give prominent notice and include the licence texts.** State that the work
   uses Qt and OpenCASCADE Technology, and include a copy of `LGPL-3.0` (plus
   `GPL-3.0`, which LGPL-3.0 incorporates by reference) and of `LGPL-2.1` with
   OCCT's exception in the package, alongside this repository's `LICENSE` and
   `LICENSING.md`.

3. **Make the corresponding library source available.** For unmodified libraries
   the upstream release is enough: pin the exact version (the vcpkg port and the
   Qt kit version both do) and either ship the source archives or include a
   written offer, valid for three years, to supply them. A URL to the exact
   upstream tarball satisfies LGPL-3.0; LGPL-2.1 section 3(b)'s written offer is
   the safer form for offline distribution. If you *modified* either library,
   the modified source is what you must offer, under that library's own licence.

   This is separate from your obligation for LuxTrace's own source. Under the
   AGPL arm you owe the complete corresponding source of the whole work anyway;
   under the commercial arm you owe none of LuxTrace's — but you still owe the
   libraries'.

   A written offer, if you use one, must reach the recipient with the binary —
   inside the package, not only on a web page. Sample wording:

   > This product includes OpenCASCADE Technology (LGPL-2.1 with exception) and
   > Qt 6 (LGPL-3.0), used as dynamically linked libraries. For a period of
   > three years from the date of this distribution, we will provide, on
   > request, a complete machine-readable copy of the corresponding source code
   > of those libraries as used in this product, on a medium customarily used
   > for software interchange, for no more than our cost of physically
   > performing the distribution. Write to: `[ADDRESS]`.

4. **Leave the recipient able to relink.** A recipient may build their own Qt or
   OCCT and run your binary against it. Do not defeat this — no signature check
   that rejects a replaced DLL, no static merge, no repacking that hides the
   import boundary. Ship the binary in a form where swapping a DLL works.

   Neither the AGPL nor the commercial licence stands in the way: the AGPL
   restricts nothing about reverse engineering, and section 3 of
   [COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md) is drafted so that its
   source-redistribution restriction does not reach the libraries.

## If you host LuxTrace instead of shipping it

Running a modified LuxTrace as a network service triggers **AGPL section 13**:
you must offer your users the complete corresponding source of your modified
version. It does not trigger the LGPL — you are not conveying Qt or OCCT to
anyone, so section 3 above stays dormant. The commercial licence removes the
section 13 obligation; see [LICENSING.md](LICENSING.md).

## Trademarks

Neither licence grants trademark rights. AGPL-3.0 section 7(e) preserves the
right to decline to grant them, and the commercial licence withholds them
expressly. "LuxTrace" and its logo remain the copyright holder's.
