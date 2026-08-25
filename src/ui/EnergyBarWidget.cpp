#include "EnergyBarWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>

EnergyBarWidget::EnergyBarWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(74);
}

QSize EnergyBarWidget::sizeHint() const        { return QSize(420, 92); }
QSize EnergyBarWidget::minimumSizeHint() const { return QSize(220, 74); }

void EnergyBarWidget::clear() {
    m_channels.clear();
    m_have = false;
    m_headline.clear();
    update();
}

void EnergyBarWidget::setResult(const SimulationResult& res) {
    m_channels.clear();
    m_power = res.sourcePower;
    m_unit  = QLatin1String(fluxUnitName(res.unit));
    m_have  = res.sourcePower > 0.0;
    if (!m_have) { update(); return; }

    const double p = res.sourcePower;
    auto add = [&](const QString& name, double flux, QColor colour, const QString& note) {
        if (std::fabs(flux) < 1e-15) return;
        m_channels.push_back({name, note, flux / p, colour, QRect()});
    };

    add(QStringLiteral("On the receiver"), res.fluxDetector, QColor(96, 200, 128),
        QStringLiteral("what the optic delivered"));
    add(QStringLiteral("Absorbed at surfaces"),
        res.fluxAbsorbed - res.fluxBulkAbsorbed, QColor(200, 96, 84),
        QStringLiteral("lost at an interface, per hit"));
    add(QStringLiteral("Absorbed in the bulk"), res.fluxBulkAbsorbed, QColor(160, 72, 64),
        QStringLiteral("Beer-Lambert, so it scales with path length"));
    add(QStringLiteral("Escaped"), res.fluxEscaped, QColor(110, 140, 200),
        QStringLiteral("left the scene without hitting anything"));
    add(QStringLiteral("Refused"), res.fluxRejected, QColor(190, 160, 90),
        QStringLiteral("outside a receiver's acceptance cone: a measurement "
                       "condition, not a loss"));
    add(QStringLiteral("Truncated"), res.fluxTruncated, QColor(150, 110, 190),
        QStringLiteral("dropped at the depth limit or a degenerate branch"));
    // Signed, and drawn as its magnitude: it is the estimator's noise made
    // visible rather than a loss channel, and its expectation is exactly zero.
    add(QStringLiteral("Estimator residual"), res.fluxRoulette, QColor(120, 120, 130),
        QStringLiteral("zero in expectation; this is how far from zero it landed"));

    m_headline = QStringLiteral("%1 %2 emitted, %3 %2 delivered (%4 %5 %6 %%)")
                     .arg(res.sourcePower, 0, 'g', 4)
                     .arg(m_unit)
                     .arg(res.fluxDetector, 0, 'g', 4)
                     .arg(100.0 * res.efficiency, 0, 'f', 2)
                     .arg(QChar(0x00B1))
                     .arg(100.0 * res.efficiencyStdErr, 0, 'f', 3);
    update();
}

void EnergyBarWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), palette().base());

    if (!m_have || m_channels.empty()) {
        p.setPen(QColor(140, 140, 150));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("Run a trace to see where the light goes"));
        return;
    }

    const int margin = 8;
    QFont small = font();
    small.setPointSizeF(std::max(7.0, font().pointSizeF() - 1.0));
    p.setFont(small);
    const int lineH = QFontMetrics(small).height();

    p.setPen(palette().text().color());
    p.drawText(margin, margin + lineH - 3, m_headline);

    const int barTop = margin + lineH + 4;
    const int barH   = std::max(14, height() - barTop - margin - lineH - 4);
    const int barW   = width() - 2 * margin;
    if (barW <= 0) return;

    // The bar is drawn over the *magnitudes*, so a negative residual still takes
    // room proportional to how far from zero it is rather than eating another
    // channel's space.
    double total = 0.0;
    for (const Channel& c : m_channels) total += std::fabs(c.value);
    if (total <= 0.0) return;

    int x = margin;
    for (Channel& c : m_channels) {
        const int w = int(std::llround(std::fabs(c.value) / total * barW));
        c.rect = QRect(x, barTop, std::max(1, w), barH);
        p.fillRect(c.rect, c.colour);
        // A share big enough to write in gets its number written in it, so the
        // common case needs no hover at all.
        if (w > 46) {
            p.setPen(QColor(255, 255, 255, 220));
            p.drawText(c.rect, Qt::AlignCenter,
                       QStringLiteral("%1%").arg(100.0 * c.value, 0, 'f', 1));
        }
        x += w;
    }
    p.setPen(QColor(0, 0, 0, 60));
    p.drawRect(QRect(margin, barTop, barW - 1, barH - 1));

    // A legend line under the bar, and the hovered channel spelled out in full.
    p.setPen(QColor(130, 130, 140));
    if (m_hover >= 0 && m_hover < int(m_channels.size())) {
        const Channel& c = m_channels[std::size_t(m_hover)];
        p.drawText(margin, barTop + barH + lineH,
                   QStringLiteral("%1  %2 %3  (%4 %%) - %5")
                       .arg(c.name)
                       .arg(c.value * m_power, 0, 'g', 4)
                       .arg(m_unit)
                       .arg(100.0 * c.value, 0, 'f', 3)
                       .arg(c.note));
    } else {
        QStringList names;
        for (const Channel& c : m_channels) names << c.name;
        p.drawText(margin, barTop + barH + lineH,
                   QStringLiteral("hover a band: %1").arg(names.join(QStringLiteral(", "))));
    }
}

void EnergyBarWidget::mouseMoveEvent(QMouseEvent* e) {
    int hit = -1;
    for (std::size_t i = 0; i < m_channels.size(); ++i)
        if (m_channels[i].rect.contains(e->pos())) { hit = int(i); break; }
    if (hit != m_hover) { m_hover = hit; update(); }
}

void EnergyBarWidget::leaveEvent(QEvent*) {
    if (m_hover != -1) { m_hover = -1; update(); }
}
