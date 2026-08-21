#pragma once
#include <QColor>
#include <QString>
#include <QStringList>

// Colour maps for the heatmap and the plots.
//
// The default is Viridis rather than the rainbow: a rainbow map has bright
// bands at yellow and cyan that read as structure in the data when there is
// none, and it collapses to mush in greyscale. Jet is kept because a lot of
// optical software uses it and a comparison against a LightTools plot is easier
// when both use the same map.
namespace palette {

enum class Map {
    Viridis = 0,
    Inferno,
    Jet,
    Grayscale,
    Turbo,
    Count
};

QStringList names();
QString     name(Map m);

// t is clamped to [0, 1].
QColor sample(Map m, double t);

// Scaling of a value into [0, 1] for display.
enum class Scale {
    Linear = 0,
    Log,        // decades below the peak, see kLogDecades
    Sqrt,       // a gentle middle ground
    Count
};

QStringList scaleNames();
QString     scaleName(Scale s);

// How many decades a log-scaled map shows before clipping to zero. Four decades
// is enough to reveal stray light an order or two below the beam without
// turning read noise into a picture.
constexpr double kLogDecades = 4.0;

// Maps `value` into [0, 1] given the peak of the data set.
double normalise(Scale s, double value, double peak);

} // namespace palette
