# Licensing

LuxTrace is **Copyright (C) 2026 Kutay Gunal** and is available under two
licences. You choose which one you take it under; you do not get both at once.

| | Open-source licence | Commercial licence |
|---|---|---|
| **Terms** | [GNU AGPL v3, *only*](LICENSE) (SPDX: `AGPL-3.0-only`) | [COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md) |
| **Cost** | Free | Paid, per seat |
| **You must publish your source** | Yes — see below | No |
| **Suits** | Research, teaching, personal use, other AGPL projects | Products, internal for-profit use, closed-source work |

## What the AGPL requires of you

The AGPL grants everything: use, modification, redistribution, commercial use.
Its price is reciprocity, and it is a stronger form of reciprocity than the GPL:

- **Distribute a modified LuxTrace, or a program that incorporates it, and you
  must release the complete corresponding source of that whole work under the
  AGPL** (§5, §6). Not just your diff against LuxTrace — everything the combined
  work is made of.
- **§13, the network clause.** If you let users interact with a modified
  LuxTrace *remotely over a network* — a hosted optical-design service, a web
  front end, an internal simulation API other teams call — you must offer those
  users the complete corresponding source, even though you never shipped them a
  binary. This is what the AGPL adds over the GPL, and it is the clause that
  closes the "run it as a service and never distribute" route.
- Keep the copyright and licence notices intact, state what you changed, and
  pass the AGPL on to anyone you give the work to.

**Version 3 only.** The grant is `AGPL-3.0-only`, not `-or-later`: it does not
extend to future versions of the AGPL that the Free Software Foundation may
publish. That keeps the terms of the open arm fixed and known.

Internal use inside a company, with no distribution and no network-facing
deployment, does not trigger these obligations. The moment either happens, it
does.

## When you need the commercial licence

Take the commercial licence if you want to do any of the following without
publishing your own source:

- Ship LuxTrace, or something built on it, in a product you distribute.
- Run a modified LuxTrace behind a network service your users reach (the §13
  case).
- Link LuxTrace into closed-source code of your own.
- Take it with a warranty, an indemnity, support, or an explicit patent grant
  written into a signed agreement — none of which the AGPL offers.

The commercial licence removes the copyleft obligations entirely. It does *not*
remove the third-party obligations that come with Qt and OpenCASCADE: those
travel with the binary under either arm. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

To obtain one, contact the copyright holder. Terms and the per-seat model are in
[COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md).

## Why dual licensing works here, and what keeps it working

Only the copyright holder can offer terms other than the AGPL. That is why:

- **Every contribution must come with a licence grant back to the copyright
  holder.** A single contributed patch held under the AGPL alone, with no such
  grant, makes the whole work impossible to relicense commercially from that
  point forward. See [CONTRIBUTING.md](CONTRIBUTING.md) and [CLA.md](CLA.md);
  no contribution is merged without it.
- **The name is not licensed by either arm.** "LuxTrace", its logo, and its
  domain are trademarks of the copyright holder. The AGPL grants copyright and
  patent rights, not trademark rights (AGPL §7(e) preserves this explicitly). A
  fork may exist; it may not call itself LuxTrace.
- **Publication is one-way.** Every version released publicly under the AGPL
  stays available under the AGPL to everyone who received it, permanently. The
  copyright holder may license *future* versions differently, or sell the
  copyright outright to a buyer who then does so — but no one, including the
  copyright holder or that buyer, can withdraw a grant already made. Publish
  only what you are content to have public forever.
