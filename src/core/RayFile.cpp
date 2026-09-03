// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "RayFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

QString RayFileData::summary() const {
    if (!valid()) return QStringLiteral("empty ray set");
    const Vec3 e = extent();
    QString s = QStringLiteral("%1 rays, %2 %3")
                    .arg(rays.size())
                    .arg(raySetFlux, 0, 'g', 4)
                    .arg(QLatin1String(fluxUnitName(unit)));
    if (declared > rays.size())
        s += QStringLiteral(" (sampled from %1)").arg(declared);
    s += QStringLiteral(", emitter %1 x %2 x %3 mm")
             .arg(e.x, 0, 'g', 3).arg(e.y, 0, 'g', 3).arg(e.z, 0, 'g', 3);
    if (hasWavelengths)          s += QStringLiteral(", per-ray wavelengths");
    else if (headerWavelengthNm > 0.0)
        s += QStringLiteral(", %1 nm").arg(headerWavelengthNm, 0, 'f', 1);
    else                         s += QStringLiteral(", no wavelengths (the run's "
                                                     "spectrum is used)");
    return s;
}

namespace rayfile {
namespace {

// Zemax source-file header. 208 bytes, little-endian, float32 throughout.
constexpr int   kZemaxIdentifier = 1010;
constexpr qint64 kZemaxHeaderBytes = 208;

// Millimetres per file unit, from the header's dimension code. Getting this
// wrong is the most common ray-file mistake there is, and it is the one the
// file itself can settle.
double unitScaleFor(qint32 code) {
    switch (code) {
    case 0: return 1.0;        // mm
    case 1: return 10.0;       // cm
    case 2: return 25.4;       // inches
    case 3: return 1000.0;     // metres
    default: return 1.0;
    }
}

// Which subset of `declared` rays to keep when a cap is in force. Evenly
// strided rather than the leading block: a measured set is often written in
// scan order, so its first N rays are one edge of the die rather than a sample
// of the whole emitter.
inline bool keepRay(std::size_t index, std::size_t declared, std::size_t cap) {
    if (cap == 0 || declared <= cap) return true;
    // index * cap / declared changes value exactly `cap` times over the range.
    return (index * cap) / declared != ((index + 1) * cap) / declared;
}

void finishBounds(RayFileData& d) {
    if (d.rays.empty()) return;
    d.bmin = d.bmax = d.rays.front().origin;
    for (const SourceRay& r : d.rays) {
        d.bmin.x = std::min(d.bmin.x, r.origin.x);
        d.bmin.y = std::min(d.bmin.y, r.origin.y);
        d.bmin.z = std::min(d.bmin.z, r.origin.z);
        d.bmax.x = std::max(d.bmax.x, r.origin.x);
        d.bmax.y = std::max(d.bmax.y, r.origin.y);
        d.bmax.z = std::max(d.bmax.z, r.origin.z);
    }
}

// The ray-set flux is recomputed from the rays actually kept rather than taken
// from the header, because a capped or partly-unreadable load holds a different
// set from the one the header describes. `sourceFlux` keeps the header's claim,
// so the two can be compared.
void finishFlux(RayFileData& d) {
    double sum = 0.0;
    for (const SourceRay& r : d.rays) sum += r.power;
    d.raySetFlux = sum;
}

const QRegularExpression& separators() {
    static const QRegularExpression re(QStringLiteral("[\\s,;]+"));
    return re;
}

} // namespace

LoadResult loadZemaxBinary(const QString& path, std::size_t maxRays) {
    LoadResult res;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        res.error = f.errorString();
        return res;
    }
    if (f.size() < kZemaxHeaderBytes) {
        res.error = QStringLiteral("%1 is too short to be a binary source file")
                        .arg(QFileInfo(path).fileName());
        return res;
    }

    QDataStream in(&f);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setFloatingPointPrecision(QDataStream::SinglePrecision);

    qint32 identifier = 0, nbrRays = 0;
    in >> identifier >> nbrRays;
    if (identifier != kZemaxIdentifier) {
        res.error = QStringLiteral("not a binary source file (identifier %1, expected %2)")
                        .arg(identifier).arg(kZemaxIdentifier);
        return res;
    }
    if (nbrRays <= 0) {
        res.error = QStringLiteral("the header declares %1 rays").arg(nbrRays);
        return res;
    }

    char description[100] = {};
    if (in.readRawData(description, 100) != 100) {
        res.error = QStringLiteral("truncated header");
        return res;
    }

    float sourceFlux = 0.0f, raySetFlux = 0.0f, wavelength = 0.0f;
    float inclBeg = 0.0f, inclEnd = 0.0f, azimBeg = 0.0f, azimEnd = 0.0f;
    qint32 dimensionUnits = 0;
    float locX = 0.0f, locY = 0.0f, locZ = 0.0f;
    float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
    float scaleX = 0.0f, scaleY = 0.0f, scaleZ = 0.0f;
    float unused[4] = {};
    qint32 rayFormat = 0, fluxType = 0, reserved1 = 0, reserved2 = 0;

    in >> sourceFlux >> raySetFlux >> wavelength
       >> inclBeg >> inclEnd >> azimBeg >> azimEnd
       >> dimensionUnits
       >> locX >> locY >> locZ
       >> rotX >> rotY >> rotZ
       >> scaleX >> scaleY >> scaleZ
       >> unused[0] >> unused[1] >> unused[2] >> unused[3]
       >> rayFormat >> fluxType >> reserved1 >> reserved2;

    if (in.status() != QDataStream::Ok) {
        res.error = QStringLiteral("truncated header");
        return res;
    }

    // Format 0 is seven floats per ray; format 2 adds a per-ray wavelength.
    // Anything else is a layout this reader would misread rather than refuse,
    // which is the worse of the two failures.
    const int perRay = (rayFormat == 2) ? 8 : 7;
    if (rayFormat != 0 && rayFormat != 2) {
        res.error = QStringLiteral("ray format %1 is not one this reader knows "
                                   "(expected 0 or 2)").arg(rayFormat);
        return res;
    }

    auto d = std::make_shared<RayFileData>();
    d->path     = path;
    d->label    = QString::fromLatin1(description, int(qstrnlen(description, 100))).trimmed();
    if (d->label.isEmpty()) d->label = QFileInfo(path).completeBaseName();
    d->format   = QStringLiteral("Zemax binary source file, ray format %1").arg(rayFormat);
    d->unit     = (fluxType == 1) ? FluxUnit::Lumen : FluxUnit::Watt;
    d->sourceFlux = sourceFlux;
    d->unitScale  = unitScaleFor(dimensionUnits);
    d->declared   = std::size_t(nbrRays);
    d->headerWavelengthNm = (wavelength > 0.0f && wavelength < 100.0f)
                                ? double(wavelength) * 1000.0   // quoted in um
                                : double(wavelength);

    const std::size_t declared = std::size_t(nbrRays);
    d->rays.reserve(maxRays > 0 ? std::min(maxRays, declared) : declared);

    // Two arguments, not one: a single `std::size_t(perRay)` parses as a
    // function declaration rather than as a vector.
    std::vector<float> buf(static_cast<std::size_t>(perRay), 0.0f);
    for (std::size_t i = 0; i < declared; ++i) {
        for (int c = 0; c < perRay; ++c) in >> buf[std::size_t(c)];
        if (in.status() != QDataStream::Ok) break;      // a truncated file is still usable
        if (!keepRay(i, declared, maxRays)) continue;

        SourceRay r;
        r.origin = Vec3(double(buf[0]), double(buf[1]), double(buf[2])) * d->unitScale;
        r.dir    = Vec3(double(buf[3]), double(buf[4]), double(buf[5]));
        r.power  = double(buf[6]);
        if (perRay == 8) {
            // Zemax quotes the per-ray wavelength in micrometres.
            const double w = double(buf[7]);
            r.wavelengthNm = (w > 0.0 && w < 100.0) ? w * 1000.0 : w;
            if (r.wavelengthNm > 0.0) d->hasWavelengths = true;
        }
        if (!r.dir.normalize() || !(r.power > 0.0)) continue;
        d->rays.push_back(r);
    }

    if (d->rays.empty()) {
        res.error = QStringLiteral("%1 carried no usable rays")
                        .arg(QFileInfo(path).fileName());
        return res;
    }

    finishBounds(*d);
    finishFlux(*d);
    // A file that declares a set flux but whose rays do not add up to it has
    // been subsetted by whoever produced it. Trust the rays, keep the claim.
    if (!(d->sourceFlux > 0.0)) d->sourceFlux = double(raySetFlux);

    res.ok   = true;
    res.data = d;
    return res;
}

LoadResult loadText(const QString& path, std::size_t maxRays) {
    LoadResult res;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        res.error = f.errorString();
        return res;
    }

    // Read twice: once to count the rows that parse, once to keep an evenly
    // strided subset of them. A ray file is tens of megabytes and the second
    // pass is a re-read rather than a re-parse of everything held in memory.
    auto countRows = [&]() {
        std::size_t n = 0;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QStringList tok =
                in.readLine().trimmed().split(separators(), Qt::SkipEmptyParts);
            if (tok.size() < 6) continue;
            bool allNumbers = true;
            for (int c = 0; c < 6 && allNumbers; ++c) tok[c].toDouble(&allNumbers);
            if (allNumbers) ++n;
        }
        return n;
    };

    const std::size_t declared = countRows();
    if (declared == 0) {
        res.error = QStringLiteral("%1 holds no rows of six or more numbers, so it is "
                                   "not a text ray file")
                        .arg(QFileInfo(path).fileName());
        return res;
    }
    f.seek(0);

    auto d = std::make_shared<RayFileData>();
    d->path     = path;
    d->label    = QFileInfo(path).completeBaseName();
    d->format   = QStringLiteral("text ray file");
    d->declared = declared;
    d->rays.reserve(maxRays > 0 ? std::min(maxRays, declared) : declared);

    QTextStream in(&f);
    std::size_t row = 0;
    int columns = 0;
    while (!in.atEnd()) {
        const QStringList tok =
            in.readLine().trimmed().split(separators(), Qt::SkipEmptyParts);
        if (tok.size() < 6) continue;

        double v[8] = {};
        bool ok = true;
        const int n = std::min(8, int(tok.size()));
        for (int c = 0; c < n && ok; ++c) v[c] = tok[c].toDouble(&ok);
        // A header line whose first six fields are not numbers is a comment,
        // whatever it calls itself.
        if (!ok) continue;

        const std::size_t here = row++;
        if (columns == 0) columns = n;
        if (!keepRay(here, declared, maxRays)) continue;

        SourceRay r;
        r.origin = Vec3(v[0], v[1], v[2]);
        r.dir    = Vec3(v[3], v[4], v[5]);
        r.power  = (n >= 7) ? v[6] : 1.0;
        if (n >= 8) {
            // A column that reads as micrometres is one: no ray file quotes a
            // visible wavelength as a number below 100 nm.
            r.wavelengthNm = (v[7] > 0.0 && v[7] < 100.0) ? v[7] * 1000.0 : v[7];
            if (r.wavelengthNm > 0.0) d->hasWavelengths = true;
        }
        if (!r.dir.normalize() || !(r.power > 0.0)) continue;
        d->rays.push_back(r);
    }

    if (d->rays.empty()) {
        res.error = QStringLiteral("%1 carried no usable rays")
                        .arg(QFileInfo(path).fileName());
        return res;
    }

    d->format = QStringLiteral("text ray file, %1 columns").arg(columns);
    finishBounds(*d);
    finishFlux(*d);
    d->sourceFlux = d->raySetFlux;

    res.ok   = true;
    res.data = d;
    return res;
}

LoadResult load(const QString& path, std::size_t maxRays) {
    // Dispatch on content, not on extension: vendors ship binary sets as .dat,
    // .ray, .txt and .dis indiscriminately, and the identifier word settles it
    // in four bytes.
    {
        QFile probe(path);
        if (probe.open(QIODevice::ReadOnly) && probe.size() >= kZemaxHeaderBytes) {
            QDataStream in(&probe);
            in.setByteOrder(QDataStream::LittleEndian);
            qint32 identifier = 0;
            in >> identifier;
            if (identifier == kZemaxIdentifier) {
                probe.close();
                return loadZemaxBinary(path, maxRays);
            }
        }
    }
    LoadResult text = loadText(path, maxRays);
    if (text.ok) return text;

    // Neither reader could make sense of it. Report the text reader's error --
    // it is the more informative of the two, since the binary one only ever
    // says "wrong identifier" for a file that is not binary at all.
    return text;
}

QString fileFilter() {
    return QStringLiteral("Source ray files (*.dat *.ray *.dis *.txt *.csv);;"
                          "Zemax / TracePro binary source (*.dat *.ray);;"
                          "ASAP or text ray file (*.dis *.txt *.csv);;"
                          "All files (*)");
}

} // namespace rayfile
