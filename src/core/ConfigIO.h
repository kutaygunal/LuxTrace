#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include "Simulation.h"
#include "SurfaceOptics.h"

namespace scenedoc { class SceneDocument; }

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

// ---- with the assembled scene alongside the configuration ------------------
//
// A composed scene is not derivable from a SimConfig: the config carries the
// *compiled* geometry, which is a pile of B-Rep solids with no memory of which
// object produced which. So the document travels beside it in the same file,
// and a config written before there were documents -- or by a run that had no
// objects -- simply carries no "document" key and loads as it always did.
QString toJson(const SimConfig& cfg, const scenedoc::SceneDocument* doc);
bool    fromJson(const QString& json, SimConfig& out, scenedoc::SceneDocument* doc,
                 QString* errorOut = nullptr, QStringList* warnings = nullptr);
bool    save(const QString& path, const SimConfig& cfg, const scenedoc::SceneDocument* doc,
             QString* errorOut = nullptr);
bool    load(const QString& path, SimConfig& out, scenedoc::SceneDocument* doc,
             QString* errorOut = nullptr, QStringList* warnings = nullptr);

// ---- the pieces a scene document shares with a configuration ---------------
//
// One surface's optical behaviour, and one emitter, as JSON. Exposed rather
// than duplicated: a document describes its objects with the same two structs a
// config already writes, and a second encoding of either would be a second
// thing to keep in step with the engine.
QJsonObject   opticsToJson(const SurfaceOptics& optics);
SurfaceOptics opticsFromJson(const QJsonObject& o, SurfaceOptics fallback = SurfaceOptics{});
QJsonObject   sourceToJson(const SourceSpec& spec);
SourceSpec    sourceFromJson(const QJsonObject& o, QStringList* warnings = nullptr);

} // namespace configio
