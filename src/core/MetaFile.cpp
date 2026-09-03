// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "MetaFile.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include "Optics.h"

namespace metafile {
namespace {

// Splits on whitespace, commas or semicolons, so a CSV export and a
// whitespace-aligned table both read without the caller having to say which.
QStringList fields(const QString& line) {
    static const QRegularExpression sep(QStringLiteral("[\\s,;]+"));
    return line.split(sep, Qt::SkipEmptyParts);
}

bool asDouble(const QString& s, double& out) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (ok) out = v;
    return ok;
}

} // namespace

Result parse(const QString& text) {
    Result r;
    // Wavelength -> per-order efficiency, built in a map so the file may list
    // its rows in any order and still come out ascending. A table that had to
    // be pre-sorted is a table somebody will hand in unsorted.
    std::map<double, std::array<double, meta::kOrderSlots>> es, ep;
    bool sawP = false;

    int lineNo = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        ++lineNo;
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;

        const QStringList f = fields(line);
        if (f.isEmpty()) continue;

        // Header keywords first: a word where a number should be is either a
        // key this reader knows or a line it must refuse rather than guess at.
        double first = 0.0;
        if (!asDouble(f.front(), first)) {
            const QString key = f.front().toLower();
            if (f.size() < 2) {
                r.error = QStringLiteral("line %1: '%2' has no value").arg(lineNo).arg(key);
                return r;
            }
            double v = 0.0;
            if (!asDouble(f[1], v)) {
                r.error = QStringLiteral("line %1: '%2' is not a number").arg(lineNo).arg(f[1]);
                return r;
            }
            if (key == QLatin1String("design_wavelength_nm")) r.designLambdaNm = v;
            else if (key == QLatin1String("period_um"))  r.periodMm = v * 1e-3;
            else if (key == QLatin1String("period_nm"))  r.periodMm = v * 1e-6;
            else if (key == QLatin1String("period_mm"))  r.periodMm = v;
            else {
                // Loudly, and by name. A key this reader does not understand may
                // be the one that mattered.
                r.error = QStringLiteral("line %1: unknown key '%2'").arg(lineNo).arg(key);
                return r;
            }
            continue;
        }

        if (f.size() < 3) {
            r.error = QStringLiteral("line %1: expected lambda, order and at least one "
                                     "efficiency").arg(lineNo);
            return r;
        }
        double orderD = 0.0;
        if (!asDouble(f[1], orderD)) {
            r.error = QStringLiteral("line %1: '%2' is not a diffraction order")
                          .arg(lineNo).arg(f[1]);
            return r;
        }
        const int order = int(std::lround(orderD));
        if (order < -meta::kMaxOrder || order > meta::kMaxOrder) {
            // Real, understood, and past what the surface carries. Counted so
            // the summary can say how much of the file was left on the floor.
            ++r.skipped;
            continue;
        }

        double vs = 0.0, vp = 0.0;
        bool   havePForRow = false;
        if (f.size() >= 6) {
            // re_s im_s re_p im_p -- a complex export, reduced to moduli squared.
            double rs = 0, is = 0, rp = 0, ip = 0;
            if (!asDouble(f[2], rs) || !asDouble(f[3], is) ||
                !asDouble(f[4], rp) || !asDouble(f[5], ip)) {
                r.error = QStringLiteral("line %1: expected four complex parts")
                              .arg(lineNo);
                return r;
            }
            vs = rs * rs + is * is;
            vp = rp * rp + ip * ip;
            havePForRow = true;
        } else {
            if (!asDouble(f[2], vs)) {
                r.error = QStringLiteral("line %1: '%2' is not an efficiency")
                              .arg(lineNo).arg(f[2]);
                return r;
            }
            if (f.size() >= 4 && asDouble(f[3], vp)) havePForRow = true;
        }
        if (!havePForRow) vp = vs;
        if (vs < 0.0 || vp < 0.0 || vs > 1.0001 || vp > 1.0001) {
            r.error = QStringLiteral("line %1: efficiency %2 is not a fraction of the "
                                     "incident power").arg(lineNo)
                          .arg(QString::number(std::max(vs, vp), 'g', 4));
            return r;
        }
        if (havePForRow && std::fabs(vs - vp) > 1e-9) sawP = true;

        const int slot = order + meta::kMaxOrder;
        es[first][std::size_t(slot)] = std::clamp(vs, 0.0, 1.0);
        ep[first][std::size_t(slot)] = std::clamp(vp, 0.0, 1.0);
        r.orders = std::max(r.orders, std::abs(order));
    }

    if (es.empty()) {
        r.error = QStringLiteral("no efficiency rows found");
        return r;
    }

    // A table whose orders sum past one at some wavelength is not a table with a
    // rounding problem, it is the wrong columns: a diffraction efficiency is a
    // share of the incident power and the shares cannot exceed it.
    for (const auto& kv : es) {
        double sum = 0.0;
        for (double v : kv.second) sum += v;
        if (sum > 1.0001) {
            r.error = QStringLiteral("the orders at %1 nm sum to %2, which is more "
                                     "power than arrived")
                          .arg(kv.first).arg(QString::number(sum, 'f', 4));
            return r;
        }
    }

    r.efficiency.samples    = int(es.size());
    r.efficiency.polarising = sawP;
    r.efficiency.lambdaNm.reserve(es.size());
    r.efficiency.value.reserve(es.size() * meta::kOrderSlots);
    r.efficiency.valueP.reserve(es.size() * meta::kOrderSlots);
    for (const auto& kv : es) {
        r.efficiency.lambdaNm.push_back(kv.first);
        for (double v : kv.second) r.efficiency.value.push_back(v);
        const auto it = ep.find(kv.first);
        for (int i = 0; i < meta::kOrderSlots; ++i)
            r.efficiency.valueP.push_back(it != ep.end() ? it->second[std::size_t(i)]
                                                         : kv.second[std::size_t(i)]);
    }

    r.wavelengths = r.efficiency.samples;
    r.ok = true;
    r.polarising = sawP;
    r.summary = QStringLiteral("%1 wavelength(s) from %2 to %3 nm, orders -%4..+%4, %5")
                    .arg(r.wavelengths)
                    .arg(r.efficiency.lambdaNm.front())
                    .arg(r.efficiency.lambdaNm.back())
                    .arg(r.orders)
                    .arg(sawP ? QStringLiteral("s and p separately")
                              : QStringLiteral("unpolarised"));
    if (r.designLambdaNm > 0.0)
        r.summary += QStringLiteral(", designed at %1 nm").arg(r.designLambdaNm);
    if (r.periodMm > 0.0)
        r.summary += QStringLiteral(", period %1 um").arg(r.periodMm * 1e3);
    if (r.skipped > 0)
        r.summary += QStringLiteral(" (%1 row(s) past order %2 were not kept)")
                         .arg(r.skipped).arg(meta::kMaxOrder);
    return r;
}

Result load(const QString& path) {
    Result r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        r.error = QStringLiteral("cannot open %1").arg(path);
        return r;
    }
    QTextStream in(&f);
    return parse(in.readAll());
}

} // namespace metafile
