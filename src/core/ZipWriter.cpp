// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "core/ZipWriter.h"

#include <QDateTime>
#include <QFile>

namespace zip {
namespace {

// Standard CRC-32 (the polynomial used by ZIP, PNG, gzip). A small table is
// built once on first use.
quint32 crc32(const QByteArray& data) {
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (char ch : data)
        crc = table[(crc ^ quint8(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// Little-endian writers for the raw fields a ZIP header carries. Built by
// explicit byte append -- deliberately not through QDataStream, whose internal
// cursor goes stale when the growing QByteArray reallocates.
void put16(QByteArray& b, quint16 v) {
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}
void put32(QByteArray& b, quint32 v) {
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 24) & 0xFF));
}

// The DOS date/time a ZIP header carries. The year is stored relative to 1980.
void dosTimeDate(const QDateTime& dt, quint16& time, quint16& date) {
    const QDate d = dt.date();
    const QTime t = dt.time();
    time = quint16((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    date = quint16(((d.year() - 1980) << 9) | (d.month() << 5) | d.day());
}

// The 30-byte local file header preceding each entry's data.
void writeLocalHeader(QByteArray& b, const QByteArray& name, const QByteArray& data,
                      quint32 crc, quint16 t, quint16 d) {
    put32(b, 0x04034b50u);                 // signature
    put16(b, 20);                          // version needed to extract
    put16(b, 0);                           // general purpose bit flag
    put16(b, 0);                           // compression method: store
    put16(b, t);
    put16(b, d);
    put32(b, crc);
    put32(b, quint32(data.size()));        // compressed size
    put32(b, quint32(data.size()));        // uncompressed size
    put16(b, quint16(name.size()));
    put16(b, 0);                           // extra field length
    b += name;
}

} // namespace

bool write(const QString& path, const QVector<Entry>& entries, QString* error) {
    QByteArray out;

    // One timestamp for the whole archive. The local header and the central
    // directory each carry a copy of an entry's mtime, and reading the clock
    // twice lets them disagree whenever a large bundle takes longer to
    // serialise than the format's two-second resolution -- which strict
    // validators read as a damaged archive.
    quint16 t, d;
    dosTimeDate(QDateTime::currentDateTime(), t, d);

    QVector<quint32> offsets;
    QVector<quint32> crcs;
    QVector<quint32> sizes;
    QVector<QByteArray> names;
    offsets.reserve(entries.size());
    crcs.reserve(entries.size());
    sizes.reserve(entries.size());
    names.reserve(entries.size());

    quint32 offset = 0;
    for (const Entry& e : entries) {
        const QByteArray name = e.name.toUtf8();
        const QByteArray data = e.data;
        const quint32 crc = crc32(data);

        offsets << offset;
        crcs << crc;
        sizes << quint32(data.size());
        names << name;

        writeLocalHeader(out, name, data, crc, t, d);
        out += data;
        offset += 30 + quint32(name.size()) + quint32(data.size());
    }

    const quint32 cdStart = offset;
    for (int i = 0; i < entries.size(); ++i) {
        put32(out, 0x02014b50u);           // central directory signature
        put16(out, 20);                    // version made by
        put16(out, 20);                    // version needed to extract
        put16(out, 0);                     // general purpose bit flag
        put16(out, 0);                     // compression method: store
        put16(out, t);
        put16(out, d);
        put32(out, crcs[i]);
        put32(out, sizes[i]);              // compressed size
        put32(out, sizes[i]);              // uncompressed size
        put16(out, quint16(names[i].size()));
        put16(out, 0);                     // extra field length
        put16(out, 0);                     // file comment length
        put16(out, 0);                     // disk number start
        put16(out, 0);                     // internal file attributes
        put32(out, 0);                     // external file attributes
        put32(out, offsets[i]);            // local header offset
        out += names[i];
    }
    const quint32 cdSize = quint32(out.size()) - cdStart;

    // End of central directory record (22 bytes).
    put32(out, 0x06054b50u);               // signature
    put16(out, 0);                         // disk number
    put16(out, 0);                         // disk with the central directory
    put16(out, quint16(entries.size()));
    put16(out, quint16(entries.size()));
    put32(out, cdSize);
    put32(out, cdStart);
    put16(out, 0);                         // comment length

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("could not open %1 for writing").arg(path);
        return false;
    }
    // A short write is a truncated archive, and a truncated archive opens in
    // nothing. A full disk, an over-quota share or a dropped network mount all
    // arrive here rather than as an open() failure, so the byte count is
    // checked and close() is asked whether it actually flushed.
    const qint64 written = f.write(out);
    const bool   flushed = f.flush();
    f.close();
    if (written != qint64(out.size()) || !flushed || f.error() != QFileDevice::NoError) {
        if (error)
            *error = QStringLiteral("could not write %1: %2")
                         .arg(path, f.errorString());
        f.remove();
        return false;
    }
    return true;
}

} // namespace zip
