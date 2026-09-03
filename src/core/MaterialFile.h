// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QString>
#include <QStringList>
#include <vector>
#include "Material.h"

// Reading published optical-material data into the catalogue.
//
// The engine shipped with ten materials hard-coded. That is a demonstration,
// not a tool: the first thing a lens designer does is type a glass name, and if
// it is not there the tool is not usable for their job whatever else it does.
//
// Two formats cover almost everything anyone actually has:
//
//   .agf   the Zemax glass-catalogue format that Schott, Ohara, CDGM, Hoya and
//          Sumita all publish their catalogues in. Line-oriented ASCII: an NM
//          record names the glass and picks a dispersion formula, CD carries
//          that formula's coefficients, LD the valid wavelength range and IT
//          the internal transmittance the bulk absorption comes from.
//
//   .yml   a refractiveindex.info entry. YAML in shape but with a fixed
//          structure -- a DATA list of blocks, each a formula with coefficients
//          or a table of samples -- so it is read as that structure rather than
//          through a general YAML parser the project does not need.
//
// Everything resolves into the same fixed-size OpticalMaterial the tracer
// already reads, at load, so nothing on the hot path changes.
namespace materialfile {

// What one load did, so the caller can report it rather than guess.
struct LoadResult {
    bool        ok = false;
    int         added   = 0;      // materials entered into the catalogue
    int         skipped = 0;      // records read but not representable
    QString     error;            // set only when `ok` is false
    QStringList names;            // what was added, in file order
};

// Reads a Zemax .agf glass catalogue. Every glass whose dispersion formula can
// be represented -- exactly where the formula is a Sellmeier, by resampling
// onto the table model where it is not -- is added to the catalogue.
LoadResult loadAgf(const QString& path);

// Reads one refractiveindex.info YAML entry. `name` overrides the catalogue
// name; empty takes the file's base name, which is how the site names them
// (Ag.yml, BK7.yml).
LoadResult loadRefractiveIndexYaml(const QString& path, const QString& name = QString());

// Dispatches on the file extension. This is what a file dialog calls.
LoadResult load(const QString& path);

// The filter string for that dialog, kept next to the readers so a new format
// is one edit rather than two.
QString fileFilter();

} // namespace materialfile
