// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "MaterialFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace materialfile {
namespace {

constexpr double kLambdaD = 587.5618;   // helium d, nm

// The band a resampled material is sampled across. Wider than the visible on
// both sides so a near-UV or near-IR trace still interpolates rather than
// clamping, and narrow enough that eight samples resolve the curve: over
// 360-900 nm a crown moves by about 0.02 in index, and eight points across that
// leave a linear interpolation good to a few units in the fifth decimal, which
// is the precision a catalogue publishes to anyway.
constexpr double kResampleMinNm = 360.0;
constexpr double kResampleMaxNm = 900.0;

double toNum(const QString& s) { return s.trimmed().toDouble(); }

const QRegularExpression& whitespace() {
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}
const QRegularExpression& separators() {
    static const QRegularExpression re(QStringLiteral("[\\s,]+"));
    return re;
}

// ---- Zemax .agf ------------------------------------------------------------

// The dispersion formulae an .agf NM record can name. Only the ones that appear
// in a real vendor catalogue are listed; anything else falls through to the
// resampling path, which needs no formula-specific code at all.
enum class AgfFormula : int {
    Schott      = 1,
    Sellmeier1  = 2,
    Herzberger  = 3,
    Sellmeier2  = 4,
    Conrady     = 5,
    Sellmeier3  = 6,
    HandbookOfOptics1 = 7,
    HandbookOfOptics2 = 8,
    Sellmeier4  = 9,
    Extended    = 10,
    Sellmeier5  = 11,
    Extended2   = 12,
};

// n(lambda) for one .agf record, evaluated from its own formula. `um` is the
// wavelength in micrometres, which is the unit every one of these is written in.
// Returns a non-positive number where the formula cannot be evaluated there --
// a Sellmeier pole, or a negative n^2 -- so the caller can decline the record
// rather than propagate a NaN into the catalogue.
double agfIndex(AgfFormula f, const double c[10], double um) {
    const double l2 = um * um;
    switch (f) {
    case AgfFormula::Schott: {
        const double n2 = c[0] + c[1] * l2
                        + c[2] / l2 + c[3] / (l2 * l2)
                        + c[4] / (l2 * l2 * l2) + c[5] / (l2 * l2 * l2 * l2);
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    case AgfFormula::Sellmeier1:
    case AgfFormula::Sellmeier3:
    case AgfFormula::Sellmeier5: {
        // Same shape, more terms: K l^2 / (l^2 - L) summed over three, four or
        // five pairs. The coefficient array is (K1, L1, K2, L2, ...).
        const int pairs = (f == AgfFormula::Sellmeier1) ? 3
                        : (f == AgfFormula::Sellmeier3) ? 4 : 5;
        double n2 = 1.0;
        for (int i = 0; i < pairs; ++i) {
            const double k = c[2 * i], L = c[2 * i + 1];
            if (k == 0.0) continue;
            const double den = l2 - L;
            if (std::fabs(den) < 1e-12) return 0.0;
            n2 += k * l2 / den;
        }
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    case AgfFormula::Sellmeier2: {
        // n^2 - 1 = A + B1 l^2 / (l^2 - L1^2) + B2 l^2 / (l^2 - L2^2)
        double n2 = 1.0 + c[0];
        for (int i = 0; i < 2; ++i) {
            const double b = c[1 + 2 * i], L = c[2 + 2 * i];
            if (b == 0.0) continue;
            const double den = l2 - L * L;
            if (std::fabs(den) < 1e-12) return 0.0;
            n2 += b * l2 / den;
        }
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    case AgfFormula::Sellmeier4: {
        // n^2 = A + B l^2 / (l^2 - C) + D l^2 / (l^2 - E)
        double n2 = c[0];
        for (int i = 0; i < 2; ++i) {
            const double b = c[1 + 2 * i], L = c[2 + 2 * i];
            if (b == 0.0) continue;
            const double den = l2 - L;
            if (std::fabs(den) < 1e-12) return 0.0;
            n2 += b * l2 / den;
        }
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    case AgfFormula::Herzberger: {
        const double L = 1.0 / (l2 - 0.028);
        return c[0] + c[1] * L + c[2] * L * L
             + c[3] * l2 + c[4] * l2 * l2 + c[5] * l2 * l2 * l2;
    }
    case AgfFormula::Conrady:
        return c[0] + c[1] / um + c[2] / std::pow(um, 3.5);
    case AgfFormula::HandbookOfOptics1: {
        const double n2 = c[0] + c[1] / (l2 - c[2]) - c[3] * l2;
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    case AgfFormula::HandbookOfOptics2: {
        const double n2 = c[0] + c[1] * l2 / (l2 - c[2]) - c[3] * l2;
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    case AgfFormula::Extended: {
        const double n2 = c[0] + c[1] * l2 + c[2] / l2 + c[3] / (l2 * l2)
                        + c[4] / std::pow(l2, 3) + c[5] / std::pow(l2, 4)
                        + c[6] / std::pow(l2, 5) + c[7] / std::pow(l2, 6);
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    case AgfFormula::Extended2: {
        const double n2 = c[0] + c[1] * l2 + c[2] / l2 + c[3] / (l2 * l2)
                        + c[4] / std::pow(l2, 3) + c[5] / std::pow(l2, 4)
                        + c[6] * l2 * l2 + c[7] * l2 * l2 * l2;
        return n2 > 0.0 ? std::sqrt(n2) : 0.0;
    }
    }
    return 0.0;
}

// One record as the reader accumulates it, before it becomes an
// OpticalMaterial.
struct AgfRecord {
    QString name;
    int     formula = 0;
    double  nd = 0.0, vd = 0.0;
    double  coeff[10] = {};
    double  lambdaMinUm = 0.0, lambdaMaxUm = 0.0;
    // Internal transmittance samples: wavelength (um), transmittance, sample
    // thickness (mm). This is where a real bulk absorption comes from, rather
    // than one number the whole scene library shares.
    std::vector<std::array<double, 3>> transmittance;

    bool valid() const { return !name.isEmpty() && formula > 0; }
};

// Beer-Lambert alpha, 1/mm, at the d line, from the internal-transmittance
// table. Catalogues quote T over 10 mm, and sometimes 25; the thickness is in
// the record, so it is read rather than assumed.
double alphaFromTransmittance(const AgfRecord& r) {
    if (r.transmittance.empty()) return 0.0;
    const double target = kLambdaD * 1e-3;
    const std::array<double, 3>* best = nullptr;
    double bestGap = 1e30;
    for (const auto& s : r.transmittance) {
        const double gap = std::fabs(s[0] - target);
        if (gap < bestGap) { bestGap = gap; best = &s; }
    }
    if (!best) return 0.0;
    const double t = (*best)[1], mm = (*best)[2];
    if (!(t > 0.0) || t >= 1.0 || !(mm > 0.0)) return 0.0;
    return -std::log(t) / mm;
}

// Turns a record into the catalogue's own struct.
//
// A Sellmeier1 record maps onto the Sellmeier model exactly -- it is the same
// three-term expression, which is why that model is in the struct at all.
// Everything else is resampled onto the table model across the band, because a
// fixed-size POD that several threads read off a shared surface cannot carry
// twelve closed forms and stay one.
bool toMaterial(const AgfRecord& r, OpticalMaterial& out) {
    if (!r.valid()) return false;
    const auto f = AgfFormula(r.formula);

    OpticalMaterial m;
    m.set   = true;
    m.alpha = alphaFromTransmittance(r);

    if (f == AgfFormula::Sellmeier1) {
        m.model = OpticalMaterial::Model::Sellmeier;
        for (int i = 0; i < 3; ++i) {
            m.sell[i]     = r.coeff[2 * i];        // B
            m.sell[3 + i] = r.coeff[2 * i + 1];    // C, um^2
        }
        m.nd = m.indexAt(kLambdaD);
        if (!(m.nd > 0.0)) return false;
    } else {
        // The record's own validity range, clipped to the band the table
        // covers. Extrapolating a published fit past where its author stood
        // behind it would invent an optic.
        double lo = kResampleMinNm, hi = kResampleMaxNm;
        if (r.lambdaMinUm > 0.0) lo = std::max(lo, r.lambdaMinUm * 1000.0);
        if (r.lambdaMaxUm > 0.0) hi = std::min(hi, r.lambdaMaxUm * 1000.0);
        if (!(hi > lo)) { lo = kResampleMinNm; hi = kResampleMaxNm; }

        m.model   = OpticalMaterial::Model::Table;
        m.samples = OpticalMaterial::kMaxSamples;
        for (int i = 0; i < m.samples; ++i) {
            const double lam = lo + (hi - lo) * double(i) / double(m.samples - 1);
            const double n   = agfIndex(f, r.coeff, lam * 1e-3);
            if (!(n > 0.0) || !std::isfinite(n)) return false;
            m.lambdaNm[i] = lam;
            m.nSample[i]  = n;
            m.kSample[i]  = 0.0;          // a catalogue glass is transparent
        }
        m.nd = m.indexAt(kLambdaD);
    }

    // The catalogue's own d-line value wins over the formula's where the two
    // disagree, because it is the number the glass is sold by and the number a
    // user will check against. The difference is in the fifth decimal when the
    // record is consistent; the guard is there for the records that are not.
    if (r.nd > 0.0 && std::fabs(r.nd - m.nd) < 0.05) m.nd = r.nd;

    out = m;
    return true;
}

QString agfDescription(const AgfRecord& r, const OpticalMaterial& m) {
    QString d = QStringLiteral("n_d %1").arg(m.indexAt(kLambdaD), 0, 'f', 4);
    const double v = r.vd > 0.0 ? r.vd : m.abbe();
    if (v > 0.0) d += QStringLiteral(", Abbe %1").arg(v, 0, 'f', 1);
    if (m.model == OpticalMaterial::Model::Sellmeier)
        d += QStringLiteral(". Sellmeier fit as published.");
    else
        d += QStringLiteral(". Dispersion formula %1, resampled onto %2 points "
                            "across the band.")
                 .arg(r.formula).arg(OpticalMaterial::kMaxSamples);
    if (m.alpha > 0.0)
        d += QStringLiteral(" Internal transmittance gives alpha %1 /mm.")
                 .arg(m.alpha, 0, 'g', 3);
    return d;
}

// ---- refractiveindex.info YAML --------------------------------------------

// The site's files are YAML in shape but fixed in structure: a DATA list whose
// entries are blocks with a `type`, a `coefficients` line or a `data` literal,
// and an optional `wavelength_range`. Reading that structure directly is a
// hundred lines; a general YAML parser is a dependency, and the project needs
// exactly one shape.
struct YamlBlock {
    QString             type;
    std::vector<double> coefficients;
    std::vector<double> lambdaUm, n, k;
    double              rangeLoUm = 0.0, rangeHiUm = 0.0;
};

std::vector<double> numbersIn(const QString& s) {
    std::vector<double> v;
    const QStringList parts = s.split(separators(), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        bool ok = false;
        const double d = p.toDouble(&ok);
        if (!ok) return {};          // a row that is not all numbers is not data
        v.push_back(d);
    }
    return v;
}

// Linear interpolation of a sample table, clamped at both ends. Past the data
// the curve is an invention, which is the same reason the built-in metals clamp.
double sampleAt(const std::vector<double>& x, const std::vector<double>& y, double at) {
    if (x.empty() || y.empty()) return 0.0;
    if (at <= x.front()) return y.front();
    if (at >= x.back())  return y.back();
    std::size_t i = 0;
    while (i + 1 < x.size() && x[i + 1] < at) ++i;
    const double span = x[i + 1] - x[i];
    if (span <= 0.0) return y[i];
    const double t = (at - x[i]) / span;
    return y[i] * (1.0 - t) + y[i + 1] * t;
}

// Loads a measured (lambda, n, optionally k) table into a material with full
// fidelity where it fits, and into the off-path origin sidecar where it does
// not.
//
// A file smaller than the struct's fixed cap is kept exactly as read: the
// hot-path arrays hold every source point, `reduced` stays false and the
// origin vectors are left empty, so the numbers a user reads are the numbers a
// vendor published. A larger file is resampled onto the cap so the hot path
// stays fixed-size, while the exact source is preserved off-path and `reduced`
// is set -- the reduction becomes visible rather than silent.
void fillTabulated(OpticalMaterial& m,
                   const std::vector<double>& lambdaUm,
                   const std::vector<double>& n,
                   const std::vector<double>& k) {
    const int src = int(lambdaUm.size());
    m.originCount = src;
    m.reduced     = false;
    m.originLambdaNm.clear();
    m.originN.clear();
    m.originK.clear();
    if (src <= 0) return;

    const bool haveExtinction = (k.size() >= std::size_t(src));
    const double lo = lambdaUm.front() * 1000.0;
    const double hi = lambdaUm.back()  * 1000.0;

    if (src <= OpticalMaterial::kMaxSamples) {
        // The whole table fits: keep it exactly as the file wrote it.
        m.model   = OpticalMaterial::Model::Table;
        m.samples = src;
        for (int i = 0; i < src; ++i) {
            m.lambdaNm[i] = lambdaUm[i] * 1000.0;
            m.nSample[i]  = n[i];
            m.kSample[i]  = haveExtinction ? k[i] : 0.0;
        }
        m.nd = m.indexAt(kLambdaD);
        return;
    }

    // Exceeds the cap: resample the hot-path arrays across the file's own range
    // (sampleAt clamps, so nothing is extrapolated), and keep the exact source
    // off-path so no point the vendor measured is lost.
    m.reduced = true;
    m.model   = OpticalMaterial::Model::Table;
    m.samples = OpticalMaterial::kMaxSamples;
    m.originLambdaNm.reserve(std::size_t(src));
    m.originN.reserve(std::size_t(src));
    m.originK.reserve(std::size_t(src));
    for (int i = 0; i < src; ++i) {
        m.originLambdaNm.push_back(lambdaUm[i] * 1000.0);
        m.originN.push_back(n[i]);
        m.originK.push_back(haveExtinction ? k[i] : 0.0);
    }
    for (int i = 0; i < m.samples; ++i) {
        const double lam = lo + (hi - lo) * double(i) / double(m.samples - 1);
        m.lambdaNm[i] = lam;
        m.nSample[i]  = sampleAt(lambdaUm, n, lam * 1e-3);
        m.kSample[i]  = k.empty() ? 0.0 : sampleAt(lambdaUm, k, lam * 1e-3);
    }
    m.nd = m.indexAt(kLambdaD);
}

// n(lambda) for a refractiveindex.info formula block. Only the two Sellmeier
// forms are evaluated: they are what the site uses for essentially every
// transparent material, and a metal is always tabulated rather than fitted.
double yamlFormulaIndex(const QString& type, const std::vector<double>& c, double um) {
    const double l2 = um * um;
    const bool squared = type.contains(QStringLiteral("formula 1"));
    const bool plain   = type.contains(QStringLiteral("formula 2"));
    if ((!squared && !plain) || c.empty()) return 0.0;

    double n2 = 1.0 + c[0];
    for (std::size_t i = 1; i + 1 < c.size(); i += 2) {
        const double b = c[i], cc = c[i + 1];
        if (b == 0.0) continue;
        // Formula 1 quotes the pole in wavelength, formula 2 in wavelength
        // squared. Getting the two the wrong way round is a silently wrong
        // glass, so they are separate branches rather than one with a flag.
        const double den = l2 - (squared ? cc * cc : cc);
        if (std::fabs(den) < 1e-12) return 0.0;
        n2 += b * l2 / den;
    }
    return n2 > 0.0 ? std::sqrt(n2) : 0.0;
}

} // namespace

LoadResult loadAgf(const QString& path) {
    LoadResult res;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        res.error = f.errorString();
        return res;
    }
    const QString src = QFileInfo(path).fileName();

    QTextStream in(&f);
    AgfRecord rec;

    auto flush = [&]() {
        if (!rec.valid()) { rec = AgfRecord{}; return; }
        OpticalMaterial m;
        if (toMaterial(rec, m)) {
            materials::add(rec.name, agfDescription(rec, m), m, src);
            res.names << rec.name;
            ++res.added;
        } else {
            ++res.skipped;
        }
        rec = AgfRecord{};
    };

    while (!in.atEnd()) {
        const QString line = in.readLine();
        const QString tag  = line.left(2).toUpper();
        // Everything after the two-letter tag, split on whitespace. The format
        // is column-free, so this is the only tokenisation it needs.
        const QStringList tok = line.mid(2).split(whitespace(), Qt::SkipEmptyParts);

        if (tag == QLatin1String("NM")) {
            flush();                              // an NM record starts a new glass
            if (tok.size() >= 2) {
                rec.name    = tok[0];
                rec.formula = int(toNum(tok[1]));
            }
            // NM <name> <formula> <MIL> <nd> <vd> ...
            if (tok.size() >= 4) rec.nd = toNum(tok[3]);
            if (tok.size() >= 5) rec.vd = toNum(tok[4]);
        } else if (tag == QLatin1String("CD")) {
            for (int i = 0; i < tok.size() && i < 10; ++i) rec.coeff[i] = toNum(tok[i]);
        } else if (tag == QLatin1String("LD")) {
            if (tok.size() >= 2) {
                rec.lambdaMinUm = toNum(tok[0]);
                rec.lambdaMaxUm = toNum(tok[1]);
            }
        } else if (tag == QLatin1String("IT")) {
            if (tok.size() >= 3)
                rec.transmittance.push_back({toNum(tok[0]), toNum(tok[1]), toNum(tok[2])});
        }
    }
    flush();

    res.ok = (res.added > 0);
    if (!res.ok && res.error.isEmpty())
        res.error = QStringLiteral("no glass records this reader could represent were "
                                   "found in %1").arg(src);
    return res;
}

LoadResult loadRefractiveIndexYaml(const QString& path, const QString& nameIn) {
    LoadResult res;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        res.error = f.errorString();
        return res;
    }
    const QFileInfo fi(path);
    const QString name = nameIn.isEmpty() ? fi.completeBaseName() : nameIn;

    QTextStream in(&f);
    std::vector<YamlBlock> blocks;
    bool inData    = false;
    bool inLiteral = false;      // inside a `data: |` block scalar

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('#'))) continue;

        // A tabulated block is a YAML literal scalar: rows of numbers indented
        // under `data: |`. Anything that parses as a full row of numbers while
        // we are inside one belongs to the current block; the first line that
        // does not ends it.
        if (inLiteral) {
            if (line.isEmpty()) continue;
            const std::vector<double> v = numbersIn(line);
            if (v.size() >= 2 && !blocks.empty()) {
                YamlBlock& b = blocks.back();
                b.lambdaUm.push_back(v[0]);
                b.n.push_back(v[1]);
                b.k.push_back(v.size() >= 3 ? v[2] : 0.0);
                continue;
            }
            inLiteral = false;
        }

        if (line.startsWith(QLatin1String("DATA"))) { inData = true; continue; }
        if (!inData) continue;

        if (line.startsWith(QLatin1String("- type:"))) {
            YamlBlock b;
            b.type = line.section(QLatin1Char(':'), 1).trimmed().toLower();
            blocks.push_back(b);
            continue;
        }
        if (blocks.empty()) continue;
        YamlBlock& b = blocks.back();

        if (line.startsWith(QLatin1String("coefficients:"))) {
            b.coefficients = numbersIn(line.section(QLatin1Char(':'), 1));
        } else if (line.startsWith(QLatin1String("wavelength_range:"))) {
            const std::vector<double> v = numbersIn(line.section(QLatin1Char(':'), 1));
            if (v.size() >= 2) { b.rangeLoUm = v[0]; b.rangeHiUm = v[1]; }
        } else if (line.startsWith(QLatin1String("data:"))) {
            inLiteral = true;
            // A one-line `data: 0.5 1.5 0.0` is legal too.
            const std::vector<double> v = numbersIn(line.section(QLatin1Char(':'), 1));
            if (v.size() >= 2) {
                b.lambdaUm.push_back(v[0]);
                b.n.push_back(v[1]);
                b.k.push_back(v.size() >= 3 ? v[2] : 0.0);
            }
        }
    }

    // A file can carry an n table and a k table as separate blocks -- which is
    // how the site publishes a metal whose two curves come from different
    // papers -- so both are found before anything is resolved.
    const YamlBlock* formula = nullptr;
    const YamlBlock* tabN    = nullptr;
    const YamlBlock* tabK    = nullptr;
    for (const YamlBlock& b : blocks) {
        if (b.type.startsWith(QLatin1String("formula")) && !b.coefficients.empty()) {
            if (!formula) formula = &b;
        } else if (b.type.contains(QLatin1String("tabulated nk")) && !b.lambdaUm.empty()) {
            if (!tabN) tabN = &b;
            if (!tabK) tabK = &b;
        } else if (b.type.contains(QLatin1String("tabulated n")) && !b.lambdaUm.empty()) {
            if (!tabN) tabN = &b;
        } else if (b.type.contains(QLatin1String("tabulated k")) && !b.lambdaUm.empty()) {
            if (!tabK) tabK = &b;
        }
    }

    OpticalMaterial m;
    m.set = true;
    QString desc;

    if (tabN) {
        // A measured table is kept exactly as a file wrote it, not forced onto a
        // fixed grid: every point fits the raised hot-path cap for ordinary
        // files, and whatever exceeds it is preserved off-path with the
        // reduction made visible rather than silent.
        if (tabN->lambdaUm.size() < 2) {
            res.error = QStringLiteral("%1 carries a table with fewer than two "
                                       "usable points").arg(fi.fileName());
            return res;
        }
        const std::vector<double> kCol = tabK ? tabK->k : std::vector<double>();
        fillTabulated(m, tabN->lambdaUm, tabN->n, kCol);
        const double lo = m.samples > 0 ? m.lambdaNm[0] : kResampleMinNm;
        const double hi = m.samples > 0 ? m.lambdaNm[m.samples - 1] : kResampleMaxNm;
        const QString fidelity = m.reduced
            ? QStringLiteral("%1 measured points, resampled onto %2 on the trace "
                             "path (exact source kept)")
                  .arg(m.originCount).arg(m.samples)
            : QStringLiteral("%1 measured points, full fidelity")
                  .arg(m.originCount);
        desc = (m.isMetal()
                   ? QStringLiteral("Measured n and k, %1-%2 nm. Reflectance comes "
                                    "from the complex Fresnel equations. %3")
                         .arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0).arg(fidelity)
                   : QStringLiteral("Measured n, %1-%2 nm. %3")
                         .arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0).arg(fidelity));
    } else if (formula) {
        double lo = kResampleMinNm, hi = kResampleMaxNm;
        if (formula->rangeLoUm > 0.0) lo = std::max(lo, formula->rangeLoUm * 1000.0);
        if (formula->rangeHiUm > 0.0) hi = std::min(hi, formula->rangeHiUm * 1000.0);
        if (!(hi > lo)) { lo = kResampleMinNm; hi = kResampleMaxNm; }

        // A refractiveindex.info "formula 1" is the Sellmeier with its pole
        // quoted in wavelength rather than in wavelength squared, so it does not
        // land on the struct's Sellmeier slots directly. Both forms resample the
        // same way.
        m.model   = OpticalMaterial::Model::Table;
        m.samples = OpticalMaterial::kMaxSamples;
        for (int i = 0; i < m.samples; ++i) {
            const double lam = lo + (hi - lo) * double(i) / double(m.samples - 1);
            const double n   = yamlFormulaIndex(formula->type, formula->coefficients,
                                                lam * 1e-3);
            if (!(n > 0.0) || !std::isfinite(n)) {
                res.error = QStringLiteral("%1: the dispersion formula could not be "
                                           "evaluated across its own stated range")
                                .arg(name);
                return res;
            }
            m.lambdaNm[i] = lam;
            m.nSample[i]  = n;
            m.kSample[i]  = 0.0;
        }
        m.nd = m.indexAt(kLambdaD);
        desc = QStringLiteral("%1, resampled onto %2 points over %3-%4 nm.")
                   .arg(formula->type).arg(OpticalMaterial::kMaxSamples)
                   .arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0);
    } else {
        res.error = QStringLiteral("%1 carries no DATA block this reader understands "
                                   "(expected a formula or a tabulated n / nk table)")
                        .arg(fi.fileName());
        return res;
    }

    if (!(m.nd > 0.0)) {
        res.error = QStringLiteral("%1 resolved to a non-physical index").arg(name);
        return res;
    }

    materials::add(name, desc, m, fi.fileName());
    res.names << name;
    res.added = 1;
    res.ok    = true;
    return res;
}

LoadResult load(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("agf")) return loadAgf(path);
    if (suffix == QLatin1String("yml") || suffix == QLatin1String("yaml"))
        return loadRefractiveIndexYaml(path);

    LoadResult res;
    res.error = QStringLiteral("%1 is not a format this reader knows. Expected a Zemax "
                               ".agf glass catalogue or a refractiveindex.info .yml "
                               "entry.").arg(QFileInfo(path).fileName());
    return res;
}

QString fileFilter() {
    return QStringLiteral("Optical materials (*.agf *.yml *.yaml);;"
                          "Zemax glass catalogue (*.agf);;"
                          "refractiveindex.info entry (*.yml *.yaml);;"
                          "All files (*)");
}

} // namespace materialfile
