// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "PlotWidget.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kLeft = 62, kRight = 14, kTop = 26, kBottom = 42;

// A "nice" tick step: 1, 2 or 5 times a power of ten, so the labels are numbers
// a person would have chosen.
double niceStep(double span, int targetTicks) {
    if (span <= 0.0 || targetTicks <= 0) return 1.0;
    const double raw   = span / double(targetTicks);
    const double mag   = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm  = raw / mag;
    double step;
    if      (norm < 1.5) step = 1.0;
    else if (norm < 3.0) step = 2.0;
    else if (norm < 7.0) step = 5.0;
    else                 step = 10.0;
    return step * mag;
}

QString fmt(double v) {
    const double a = std::fabs(v);
    if (a != 0.0 && (a < 1e-3 || a >= 1e5)) return QString::number(v, 'g', 3);
    if (a >= 100.0) return QString::number(v, 'f', 0);
    if (a >= 1.0)   return QString::number(v, 'f', 2);
    return QString::number(v, 'f', 4);
}

} // namespace

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(260, 180);
    setMouseTracking(true);
}

void PlotWidget::setTitle(const QString& title) { m_title = title; update(); }

void PlotWidget::setAxisLabels(const QString& xLabel, const QString& yLabel) {
    m_xLabel = xLabel;
    m_yLabel = yLabel;
    update();
}

void PlotWidget::setSeries(std::vector<PlotSeries> series) {
    m_series = std::move(series);
    update();
}

void PlotWidget::setMarkers(std::vector<PlotMarker> markers) {
    m_markers = std::move(markers);
    update();
}

void PlotWidget::setLogY(bool on) { m_logY = on; update(); }

void PlotWidget::setPlaceholder(const QString& text) { m_placeholder = text; update(); }

void PlotWidget::clear() {
    m_series.clear();
    m_markers.clear();
    update();
}

QPixmap PlotWidget::renderToPixmap(const QSize& size, double devicePixelRatio) const {
    QPixmap pm(QSize(int(size.width() * devicePixelRatio), int(size.height() * devicePixelRatio)));
    pm.setDevicePixelRatio(devicePixelRatio);
    pm.fill(QColor(18, 18, 22));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintTo(p, QRect(QPoint(0, 0), size), /*showHover=*/false);
    return pm;
}

void PlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(18, 18, 22));
    paintTo(p, rect(), /*showHover=*/true);
}

void PlotWidget::mouseMoveEvent(QMouseEvent* e) {
    m_hover = e->pos();
    m_hovering = true;
    update();
}

void PlotWidget::leaveEvent(QEvent*) {
    m_hovering = false;
    update();
}

void PlotWidget::paintTo(QPainter& p, const QRect& area, bool showHover) const {
    const QColor axisColor(110, 112, 122);
    const QColor gridColor(44, 46, 54);
    const QColor textColor(178, 182, 194);

    if (!m_title.isEmpty()) {
        p.setPen(QColor(220, 224, 235));
        p.drawText(QRect(area.left(), area.top() + 4, area.width(), 18),
                   Qt::AlignHCenter | Qt::AlignVCenter, m_title);
    }

    // Nothing to draw: say so rather than leaving a blank panel.
    bool anyPoint = false;
    for (const auto& s : m_series) if (!s.x.empty()) { anyPoint = true; break; }
    if (!anyPoint) {
        p.setPen(QColor(130, 134, 145));
        p.drawText(area, Qt::AlignCenter, m_placeholder);
        return;
    }

    const QRect plot(area.left() + kLeft, area.top() + kTop,
                     std::max(10, area.width() - kLeft - kRight),
                     std::max(10, area.height() - kTop - kBottom));

    // ---- data range --------------------------------------------------------
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (const auto& s : m_series) {
        for (std::size_t i = 0; i < s.x.size() && i < s.y.size(); ++i) {
            x0 = std::min(x0, s.x[i]);
            x1 = std::max(x1, s.x[i]);
            const double e = (i < s.yErr.size()) ? s.yErr[i] : 0.0;
            y0 = std::min(y0, s.y[i] - e);
            y1 = std::max(y1, s.y[i] + e);
        }
    }
    for (const auto& m : m_markers) { x0 = std::min(x0, m.x); x1 = std::max(x1, m.x); }
    if (!(x1 > x0)) { x0 -= 0.5; x1 += 0.5; }

    if (m_logY) {
        // Floor the range four decades below the top, so a single zero sample
        // cannot stretch the axis to nothing.
        y1 = std::max(y1, 1e-300);
        const double top = std::log10(std::max(y1, 1e-300));
        y0 = std::pow(10.0, top - 4.0);
        y1 = std::pow(10.0, top);
    } else {
        if (!(y1 > y0)) { y0 -= 0.5; y1 += 0.5; }
        const double pad = 0.06 * (y1 - y0);
        y0 -= pad;
        y1 += pad;
        if (y0 > 0.0 && y0 < 0.25 * y1) y0 = 0.0;   // keep zero in frame when close
    }

    auto sx = [&](double v) {
        return plot.left() + (v - x0) / (x1 - x0) * plot.width();
    };
    auto sy = [&](double v) {
        if (m_logY) {
            const double lv = std::log10(std::max(v, y0 * 1e-6 + 1e-300));
            const double l0 = std::log10(y0), l1 = std::log10(y1);
            return plot.bottom() - std::clamp((lv - l0) / (l1 - l0), 0.0, 1.0) * plot.height();
        }
        return plot.bottom() - (v - y0) / (y1 - y0) * plot.height();
    };

    // ---- grid and ticks ----------------------------------------------------
    p.setFont(QFont(font().family(), 8));
    const QFontMetrics fm(p.font());

    const double xStep = niceStep(x1 - x0, std::max(2, plot.width() / 90));
    for (double v = std::ceil(x0 / xStep) * xStep; v <= x1 + 1e-9; v += xStep) {
        const double px = sx(v);
        p.setPen(gridColor);
        p.drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom()));
        p.setPen(textColor);
        p.drawText(QRectF(px - 45, plot.bottom() + 4, 90, 14),
                   Qt::AlignHCenter | Qt::AlignTop, fmt(v));
    }

    if (m_logY) {
        for (double d = std::floor(std::log10(y0)); d <= std::log10(y1) + 1e-9; d += 1.0) {
            const double v = std::pow(10.0, d);
            if (v < y0 || v > y1) continue;
            const double py = sy(v);
            p.setPen(gridColor);
            p.drawLine(QPointF(plot.left(), py), QPointF(plot.right(), py));
            p.setPen(textColor);
            p.drawText(QRectF(area.left() + 2, py - 8, kLeft - 8, 16),
                       Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("1e%1").arg(int(d)));
        }
    } else {
        const double yStep = niceStep(y1 - y0, std::max(2, plot.height() / 44));
        for (double v = std::ceil(y0 / yStep) * yStep; v <= y1 + 1e-9; v += yStep) {
            const double py = sy(v);
            p.setPen(gridColor);
            p.drawLine(QPointF(plot.left(), py), QPointF(plot.right(), py));
            p.setPen(textColor);
            p.drawText(QRectF(area.left() + 2, py - 8, kLeft - 8, 16),
                       Qt::AlignRight | Qt::AlignVCenter, fmt(v));
        }
    }

    p.setPen(axisColor);
    p.drawRect(plot);

    if (!m_xLabel.isEmpty()) {
        p.setPen(textColor);
        p.drawText(QRect(plot.left(), plot.bottom() + 20, plot.width(), 16),
                   Qt::AlignHCenter | Qt::AlignTop, m_xLabel);
    }
    if (!m_yLabel.isEmpty()) {
        p.save();
        p.translate(area.left() + 12, plot.center().y());
        p.rotate(-90);
        p.setPen(textColor);
        p.drawText(QRect(-plot.height() / 2, -10, plot.height(), 16),
                   Qt::AlignHCenter | Qt::AlignVCenter, m_yLabel);
        p.restore();
    }

    // ---- reference lines ---------------------------------------------------
    for (const auto& m : m_markers) {
        const double px = sx(m.x);
        if (px < plot.left() - 1 || px > plot.right() + 1) continue;
        p.setPen(QPen(m.color, 1.0, Qt::DashLine));
        p.drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom()));
        if (!m.label.isEmpty()) {
            p.setPen(m.color);
            const int w = fm.horizontalAdvance(m.label) + 6;
            const bool flip = px + w > plot.right();
            p.drawText(QRectF(flip ? px - w - 2 : px + 3, plot.top() + 2, w, 14),
                       Qt::AlignLeft | Qt::AlignTop, m.label);
        }
    }

    // ---- series ------------------------------------------------------------
    p.setClipRect(plot.adjusted(1, 1, -1, -1));
    for (const auto& s : m_series) {
        const std::size_t n = std::min(s.x.size(), s.y.size());
        if (n == 0) continue;

        if (s.filled) {
            QPainterPath path;
            path.moveTo(sx(s.x[0]), sy(m_logY ? y0 : std::max(y0, 0.0)));
            for (std::size_t i = 0; i < n; ++i) path.lineTo(sx(s.x[i]), sy(s.y[i]));
            path.lineTo(sx(s.x[n - 1]), sy(m_logY ? y0 : std::max(y0, 0.0)));
            path.closeSubpath();
            QColor fill = s.color;
            fill.setAlpha(52);
            p.fillPath(path, fill);
        }

        // Error bars first, so the curve stays on top of them.
        if (!s.yErr.empty()) {
            QColor bar = s.color;
            bar.setAlpha(150);
            p.setPen(QPen(bar, 1.0));
            for (std::size_t i = 0; i < n && i < s.yErr.size(); ++i) {
                const double px = sx(s.x[i]);
                const double lo = sy(s.y[i] - s.yErr[i]);
                const double hi = sy(s.y[i] + s.yErr[i]);
                p.drawLine(QPointF(px, lo), QPointF(px, hi));
                p.drawLine(QPointF(px - 3, lo), QPointF(px + 3, lo));
                p.drawLine(QPointF(px - 3, hi), QPointF(px + 3, hi));
            }
        }

        p.setPen(QPen(s.color, 1.6));
        QPainterPath line;
        line.moveTo(sx(s.x[0]), sy(s.y[0]));
        for (std::size_t i = 1; i < n; ++i) line.lineTo(sx(s.x[i]), sy(s.y[i]));
        p.drawPath(line);

        if (s.markers) {
            p.setBrush(s.color);
            for (std::size_t i = 0; i < n; ++i)
                p.drawEllipse(QPointF(sx(s.x[i]), sy(s.y[i])), 2.6, 2.6);
            p.setBrush(Qt::NoBrush);
        }
    }
    p.setClipping(false);

    // ---- legend ------------------------------------------------------------
    int named = 0;
    for (const auto& s : m_series) if (!s.name.isEmpty()) ++named;
    if (named > 1) {
        int y = plot.top() + 4;
        for (const auto& s : m_series) {
            if (s.name.isEmpty()) continue;
            const int w = fm.horizontalAdvance(s.name) + 22;
            p.setPen(QPen(s.color, 2.0));
            p.drawLine(plot.right() - w, y + 7, plot.right() - w + 14, y + 7);
            p.setPen(textColor);
            p.drawText(QRect(plot.right() - w + 18, y, w, 14),
                       Qt::AlignLeft | Qt::AlignVCenter, s.name);
            y += 15;
        }
    }

    // ---- hover readout -----------------------------------------------------
    if (showHover && m_hovering && plot.contains(m_hover) && (x1 > x0)) {
        const double xv = x0 + (m_hover.x() - plot.left()) / double(plot.width()) * (x1 - x0);
        p.setPen(QPen(QColor(200, 200, 210, 110), 1.0, Qt::DotLine));
        p.drawLine(m_hover.x(), plot.top(), m_hover.x(), plot.bottom());

        QStringList lines;
        lines << QStringLiteral("%1 = %2").arg(m_xLabel.isEmpty() ? QStringLiteral("x") : m_xLabel,
                                               fmt(xv));
        for (const auto& s : m_series) {
            const std::size_t n = std::min(s.x.size(), s.y.size());
            if (n == 0) continue;
            // Nearest sample in x; the series are dense enough that
            // interpolating would only invent precision.
            std::size_t best = 0;
            double bestD = 1e300;
            for (std::size_t i = 0; i < n; ++i) {
                const double d = std::fabs(s.x[i] - xv);
                if (d < bestD) { bestD = d; best = i; }
            }
            lines << QStringLiteral("%1 %2").arg(s.name.isEmpty() ? QStringLiteral("y") : s.name,
                                                 fmt(s.y[best]));
        }

        int w = 0;
        for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const int h = int(lines.size()) * (fm.height() + 1) + 8;
        int bx = m_hover.x() + 10;
        if (bx + w + 12 > plot.right()) bx = m_hover.x() - w - 22;
        const int by = std::min(m_hover.y() + 10, plot.bottom() - h - 2);
        const QRect box(bx, by, w + 12, h);
        p.fillRect(box, QColor(30, 32, 40, 235));
        p.setPen(QColor(80, 84, 96));
        p.drawRect(box);
        p.setPen(QColor(215, 219, 230));
        int ty = by + 4;
        for (const auto& l : lines) {
            p.drawText(QRect(bx + 6, ty, w, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, l);
            ty += fm.height() + 1;
        }
    }
}
