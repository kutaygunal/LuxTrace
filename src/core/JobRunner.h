// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>

// A headless scripting surface over the optics core.
//
// The desktop app drives everything through panels; the command line flags in
// main.cpp each expose one study. Both are wrong for a second program that
// wants to *talk* to LuxTrace -- an optimisation loop, a test bench, a Python
// script. They read argv, print human text, and run one thing then exit.
//
// The job runner is the missing middle: operations in as JSON, results out as
// JSON, both over plain stdio so any language with a pipe can drive them. Two
// entrances share one dispatcher:
//
//   --job file   batch -- one job document, one envelope per operation,
//                then exit. The file is a document that holds an "ops" array
//                (or is itself one operation).
//   --serve      interactive -- one operation per stdin line, one envelope per
//                line, until EOF. This is what the Python client uses: a
//                process is started once and stays warm across calls.
//
// Envelope shape, one JSON object per line on stdout:
//
//   {"v":1,"seq":0,"id":7,"type":"result","op":"run","data":{...},"ms":1823.4}
//   {"v":1,"seq":1,"id":8,"type":"error","op":"optimise",
//    "error":{"code":"invalid_metric","message":"..."}}
//   {"v":1,"type":"fatal","error":{"code":"bad_job","message":"..."}}
//
// stdout carries JSON and nothing else. Everything human goes to stderr, so a
// host process can pipe stdout without any prose finding its way into a parser.
namespace jobrunner {

// Version of the wire protocol, stamped on every envelope.
inline constexpr int kProtocolVersion = 1;

// Builds the envelope for one operation. Never throws; every failure inside an
// operation comes back as an error envelope so a batch of ten runs is not
// spoiled by the third one having a typo.
QJsonObject runOne(const QJsonObject& op, int seq,
                   bool* fatalOut, QString* fatalMessageOut);

// --job [path]: "-" (or no path) reads the job document from stdin.
int runJobFile(const QString& path);

// --serve: line-per-operation loop over stdin until EOF.
int runServe();

// The operations this build understands, for the "features" operation and
// for the client's capability check.
QStringList opNames();

} // namespace jobrunner