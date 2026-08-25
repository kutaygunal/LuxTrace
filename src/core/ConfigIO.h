#pragma once
#include <QString>
#include <QStringList>
#include "Simulation.h"

// Saving and restoring a SimConfig as JSON.
//
// Scenes are stored by *name*, not by enum index: the registry is an ordered
// list that new scenes get inserted into, and an index would silently reopen a
// saved setup against the wrong optic.
namespace configio {

QString toJson(const SimConfig& cfg);

// Parses a config. Anything missing keeps its default, and every number is
// clamped to its declared range, so a truncated or hand-edited file loads as a
// usable configuration rather than failing or producing degenerate geometry.
// Returns false only when the text is not JSON at all.
//
// `warnings` collects everything the load recovered from rather than failed on:
// a ray file that has moved, or one that now holds a different number of rays
// from the set the config was written against. These are exactly the cases
// where silence would leave a user tracing an emitter they did not choose.
bool fromJson(const QString& json, SimConfig& out, QString* errorOut = nullptr,
              QStringList* warnings = nullptr);

bool save(const QString& path, const SimConfig& cfg, QString* errorOut = nullptr);
bool load(const QString& path, SimConfig& out, QString* errorOut = nullptr,
          QStringList* warnings = nullptr);

} // namespace configio
