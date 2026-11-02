#include "Formats.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

#include "updater/Signature.h"

namespace TreeUpdater
{

namespace {

const int MaxKeys = 4;
const int MaxMirrors = 16;
const int MaxSignatureLines = 8;
const int MaxPath = 512;
const int MaxVariantId = 64;
// following links further than this counts as a loop
const int MaxLinkDepth = 40;

struct Invalid
{
    QString message;
};

[[noreturn]] void fail(const QString &message)
{
    throw Invalid{message};
}

bool matches(const QString &pattern, const QString &value)
{
    QRegularExpression re(QString("\\A(?:%1)\\z").arg(pattern));
    return re.match(value).hasMatch();
}

const char *Hex64 = "[0-9a-f]{64}";
const char *Commit = "[0-9a-f]{40}";
const char *Time = "[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z";
const char *Version = "(0|[1-9][0-9]*)(\\.(0|[1-9][0-9]*))*";
const char *VariantId = "(windows|macos|linux)-(x86_64|x86|arm64)-[a-z0-9.]+(-[a-z0-9.]+)*";

QJsonObject parseObject(const QByteArray &data, int limit)
{
    if(data.size() > limit)
    {
        fail(QString("%1 bytes, more than %2").arg(data.size()).arg(limit));
    }
    if(data.startsWith("\xef\xbb\xbf"))
    {
        fail("starts with a byte order mark");
    }
    // QJsonDocument would replace invalid UTF-8 instead of refusing it
    auto text = QString::fromUtf8(data);
    if(text.toUtf8() != data)
    {
        fail("not UTF-8");
    }
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(data, &error);
    if(error.error != QJsonParseError::NoError)
    {
        fail(QString("not JSON: %1 at %2").arg(error.errorString()).arg(error.offset));
    }
    if(!doc.isObject())
    {
        fail("not a JSON object");
    }
    return doc.object();
}

QJsonObject object(const QJsonValue &value, const QString &where)
{
    if(!value.isObject())
    {
        fail(QString("%1: not an object").arg(where));
    }
    return value.toObject();
}

void fields(const QJsonObject &obj, const QString &where, const QStringList &names)
{
    QStringList missing;
    for(auto &name: names)
    {
        if(!obj.contains(name))
        {
            missing.append(name);
        }
    }
    if(!missing.isEmpty())
    {
        missing.sort();
        fail(QString("%1: missing %2").arg(where, missing.join(", ")));
    }
    QStringList unknown;
    for(auto &key: obj.keys())
    {
        if(!names.contains(key))
        {
            unknown.append(key);
        }
    }
    if(!unknown.isEmpty())
    {
        unknown.sort();
        fail(QString("%1: unknown %2").arg(where, unknown.join(", ")));
    }
}

qint64 integer(const QJsonValue &value, const QString &where, qint64 minimum = 0)
{
    if(!value.isDouble())
    {
        fail(QString("%1: not an integer").arg(where));
    }
    double d = value.toDouble();
    if(std::floor(d) != d)
    {
        fail(QString("%1: not an integer").arg(where));
    }
    if(d < double(minimum) || d > double(MaxInteger))
    {
        fail(QString("%1: outside %2 to %3").arg(where).arg(minimum).arg(MaxInteger));
    }
    return qint64(d);
}

QString string(const QJsonValue &value, const QString &where, const char *pattern = nullptr)
{
    if(!value.isString())
    {
        fail(QString("%1: not a string").arg(where));
    }
    auto s = value.toString();
    if(pattern && !matches(pattern, s))
    {
        fail(QString("%1: '%2' isn't valid").arg(where, s));
    }
    return s;
}

QJsonArray list(const QJsonValue &value, const QString &where, qint64 minimum, qint64 maximum)
{
    if(!value.isArray())
    {
        fail(QString("%1: not a list").arg(where));
    }
    auto array = value.toArray();
    if(array.size() < minimum || array.size() > maximum)
    {
        fail(QString("%1: %2 entries, allowed %3 to %4").arg(where).arg(array.size()).arg(minimum).arg(maximum));
    }
    return array;
}

QDateTime time(const QJsonValue &value, const QString &where)
{
    auto s = string(value, where, Time);
    // date and time apart, so local time (and its daylight saving gaps) never comes into it
    QDateTime t(QDate::fromString(s.left(10), "yyyy-MM-dd"), QTime::fromString(s.mid(11, 8), "HH:mm:ss"), Qt::UTC);
    if(!t.date().isValid() || !t.time().isValid())
    {
        fail(QString("%1: '%2' isn't a valid time").arg(where, s));
    }
    return t;
}

QStringList keys(const QJsonValue &value, const QString &where)
{
    auto array = list(value, where, 1, MaxKeys);
    QStringList out;
    for(int i = 0; i < array.size(); i++)
    {
        auto key = string(array[i], QString("%1[%2]").arg(where).arg(i), Hex64);
        if(out.contains(key))
        {
            fail(QString("%1: a key is listed twice").arg(where));
        }
        out.append(key);
    }
    return out;
}

void header(const QJsonObject &doc, const QString &type, QStringList names)
{
    // what it is and which format first, so a wrong document says so rather than what it lacks
    if(doc.value("type") != QJsonValue(type))
    {
        fail(QString("type isn't '%1'").arg(type));
    }
    auto format = doc.value("format");
    if(!format.isDouble() || format.toDouble() != 1.0)
    {
        fail("format isn't supported (only 1)");
    }
    names << "type" << "format";
    fields(doc, type, names);
}

QStringList pathParts(const QJsonValue &value, const QString &where, bool dotdot = false)
{
    auto path = string(value, where);
    if(!isValidPath(path, dotdot))
    {
        fail(QString("%1: '%2' isn't a valid path").arg(where, path));
    }
    return path.split('/');
}

SystemRequirements::Requirements requirements(const QJsonValue &value, const QString &where)
{
    auto req = object(value, where);
    auto os = req.value("os").toString();
    QStringList names;
    if(os == "windows" || os == "macos")
    {
        names << "os" << "arch" << "os_version";
    }
    else if(os == "linux")
    {
        names << "os" << "arch" << "glibc" << "glibcxx" << "qt";
    }
    else
    {
        fail(QString("%1.os: must be one of linux, macos, windows").arg(where));
    }
    fields(req, where, names);

    SystemRequirements::Requirements out;
    out.os = os;
    out.arch = string(req.value("arch"), where + ".arch", "x86|x86_64|arm64");
    if(os == "linux")
    {
        out.glibc = string(req.value("glibc"), where + ".glibc", Version);
        out.glibcxx = string(req.value("glibcxx"), where + ".glibcxx", Version);
        out.qt = string(req.value("qt"), where + ".qt", Version);
    }
    else
    {
        out.osVersion = string(req.value("os_version"), where + ".os_version", Version);
    }
    return out;
}

// where a link points, followed through other links: the path inside the installation
QString resolveLink(const QString &linkPath, const QMap<QString, QString> &links, const QString &where)
{
    auto current = linkPath.split('/');
    current.removeLast();
    auto pending = links.value(linkPath).split('/');
    int depth = 0;
    while(!pending.isEmpty())
    {
        auto part = pending.takeFirst();
        if(part == "..")
        {
            if(current.isEmpty())
            {
                fail(QString("%1: points outside the installation").arg(where));
            }
            current.removeLast();
            continue;
        }
        current.append(part);
        auto here = current.join('/');
        if(links.contains(here))
        {
            if(++depth > MaxLinkDepth)
            {
                fail(QString("%1: following the links loops").arg(where));
            }
            current.removeLast();
            pending = links.value(here).split('/') + pending;
        }
    }
    return current.join('/');
}

Variant variant(const QString &id, const QJsonValue &value, const QString &where)
{
    auto obj = object(value, where);
    fields(obj, where, {"supersedes", "requires", "files", "links"});

    Variant out;
    out.id = id;
    out.requirements = requirements(obj.value("requires"), where + ".requires");
    QRegularExpression idParts(QString("\\A(?:%1)\\z").arg(VariantId));
    auto m = idParts.match(id);
    if(m.captured(1) != out.requirements.os || m.captured(2) != out.requirements.arch)
    {
        fail(QString("%1: the ID doesn't match requires").arg(where));
    }

    auto supersedes = list(obj.value("supersedes"), where + ".supersedes", 0, MaxInteger);
    for(int i = 0; i < supersedes.size(); i++)
    {
        auto w = QString("%1.supersedes[%2]").arg(where).arg(i);
        auto other = string(supersedes[i], w, VariantId);
        if(other.size() > MaxVariantId)
        {
            fail(QString("%1: longer than %2 characters").arg(w).arg(MaxVariantId));
        }
        if(other == id)
        {
            fail(QString("%1: supersedes itself").arg(where));
        }
        if(out.supersedes.contains(other))
        {
            fail(QString("%1.supersedes: a variant is listed twice").arg(where));
        }
        out.supersedes.append(other);
    }

    QStringList paths;
    auto files = list(obj.value("files"), where + ".files", 1, MaxInteger);
    for(int i = 0; i < files.size(); i++)
    {
        auto w = QString("%1.files[%2]").arg(where).arg(i);
        auto entry = object(files[i], w);
        fields(entry, w, {"path", "sha256", "size", "executable"});
        FileEntry file;
        pathParts(entry.value("path"), w + ".path");
        file.path = entry.value("path").toString();
        file.sha256 = string(entry.value("sha256"), w + ".sha256", Hex64);
        file.size = integer(entry.value("size"), w + ".size");
        if(!entry.value("executable").isBool())
        {
            fail(QString("%1.executable: not true or false").arg(w));
        }
        file.executable = entry.value("executable").toBool();
        out.files.append(file);
        paths.append(file.path);
    }

    QMap<QString, QString> links;
    QStringList linkPaths;
    auto linkList = list(obj.value("links"), where + ".links", 0, MaxInteger);
    if(!linkList.isEmpty() && out.requirements.os == "windows")
    {
        fail(QString("%1: a Windows variant has no links").arg(where));
    }
    for(int i = 0; i < linkList.size(); i++)
    {
        auto w = QString("%1.links[%2]").arg(where).arg(i);
        auto entry = object(linkList[i], w);
        fields(entry, w, {"path", "target"});
        LinkEntry link;
        pathParts(entry.value("path"), w + ".path");
        pathParts(entry.value("target"), w + ".target", true);
        link.path = entry.value("path").toString();
        link.target = entry.value("target").toString();
        out.links.append(link);
        links.insert(link.path, link.target);
        linkPaths.append(link.path);
    }

    // sorted by path in byte order; the paths are ASCII, so QString's order is the same
    for(auto listed: {paths, linkPaths})
    {
        auto sorted = listed;
        std::sort(sorted.begin(), sorted.end());
        if(listed != sorted)
        {
            fail(QString("%1: not sorted by path").arg(where));
        }
    }

    // one spelling per path and directory, compared case-insensitively; nothing is both a
    // file or link and a directory (so no path goes through a link)
    auto entries = paths + linkPaths;
    QMap<QString, QString> spelling;
    QSet<QString> directories;
    for(auto &path: entries)
    {
        auto parts = path.split('/');
        for(int n = 1; n <= parts.size(); n++)
        {
            auto prefix = QStringList(parts.mid(0, n)).join('/');
            if(n < parts.size())
            {
                directories.insert(prefix);
            }
            auto lower = prefix.toLower();
            if(!spelling.contains(lower))
            {
                spelling.insert(lower, prefix);
            }
            else if(spelling.value(lower) != prefix)
            {
                fail(QString("%1: '%2' and '%3' differ only in case").arg(where, spelling.value(lower), prefix));
            }
        }
    }
    auto entrySet = QSet<QString>::fromList(entries);
    if(entrySet.size() != entries.size())
    {
        fail(QString("%1: a path is listed twice").arg(where));
    }
    if(!QSet<QString>(directories).intersect(entrySet).isEmpty())
    {
        fail(QString("%1: a listed path is also a directory of other paths").arg(where));
    }

    auto fileSet = QSet<QString>::fromList(paths);
    for(auto &path: linkPaths)
    {
        auto target = resolveLink(path, links, QString("%1: link '%2'").arg(where, path));
        if(!fileSet.contains(target) && !directories.contains(target))
        {
            fail(QString("%1: link '%2' points to '%3', which isn't a listed file or a directory with files").arg(where, path, target));
        }
    }
    return out;
}

template <typename T>
bool guarded(QString &error, T body)
{
    try
    {
        body();
        return true;
    }
    catch(const Invalid &e)
    {
        error = e.message;
        return false;
    }
}

}

bool isValidPath(const QString &path, bool dotdot)
{
    if(path.isEmpty() || path.size() > MaxPath)
    {
        return false;
    }
    for(auto c: path)
    {
        if(c < QChar(0x20) || c > QChar(0x7e) || QString("\\:*?\"<>|").contains(c))
        {
            return false;
        }
    }
    for(auto &part: path.split('/'))
    {
        if(part.isEmpty() || part == "." || (part == ".." && !dotdot))
        {
            return false;
        }
    }
    return true;
}

QString contentPath(const QString &sha256)
{
    return QString("files/%1/%2").arg(sha256.left(2), sha256);
}

bool parseKeys(const QByteArray &data, qint64 expected, KeysDocument &out, QString &error)
{
    return guarded(error, [&]()
    {
        auto doc = parseObject(data, MaxDocumentSize);
        header(doc, "keys", {"version", "expires", "root_keys", "online_keys"});
        KeysDocument keysDoc;
        keysDoc.version = integer(doc.value("version"), "version", 1);
        if(keysDoc.version != expected)
        {
            fail(QString("version is %1, but the file is for version %2").arg(keysDoc.version).arg(expected));
        }
        keysDoc.expires = time(doc.value("expires"), "expires");
        keysDoc.rootKeys = keys(doc.value("root_keys"), "root_keys");
        keysDoc.onlineKeys = keys(doc.value("online_keys"), "online_keys");
        for(auto &key: keysDoc.rootKeys)
        {
            if(keysDoc.onlineKeys.contains(key))
            {
                fail("a key is both a root key and an online key");
            }
        }
        out = keysDoc;
    });
}

bool parseTags(const QByteArray &data, TagsDocument &out, QString &error)
{
    return guarded(error, [&]()
    {
        auto doc = parseObject(data, MaxDocumentSize);
        header(doc, "tags", {"serial", "issued", "tags", "withdrawn"});
        TagsDocument tags;
        tags.serial = integer(doc.value("serial"), "serial", 1);
        tags.issued = time(doc.value("issued"), "issued");
        auto tagObj = object(doc.value("tags"), "tags");
        fields(tagObj, "tags", {"latest", "testing", "stable"});
        tags.latest = integer(tagObj.value("latest"), "tags.latest", 1);
        tags.testing = integer(tagObj.value("testing"), "tags.testing", 1);
        tags.stable = integer(tagObj.value("stable"), "tags.stable", 1);
        if(!(tags.stable <= tags.testing && tags.testing <= tags.latest))
        {
            fail(QString("tags: need stable <= testing <= latest, have %1, %2, %3").arg(tags.stable).arg(tags.testing).arg(tags.latest));
        }
        auto withdrawn = list(doc.value("withdrawn"), "withdrawn", 0, MaxInteger);
        for(int i = 0; i < withdrawn.size(); i++)
        {
            auto build = integer(withdrawn[i], QString("withdrawn[%1]").arg(i), 1);
            if(!tags.withdrawn.isEmpty() && tags.withdrawn.last() >= build)
            {
                fail("withdrawn: not ascending, or a build is listed twice");
            }
            if(build == tags.latest || build == tags.testing || build == tags.stable)
            {
                fail(QString("withdrawn: build %1 is tagged").arg(build));
            }
            tags.withdrawn.append(build);
        }
        out = tags;
    });
}

bool parseMirrors(const QByteArray &data, MirrorsDocument &out, QString &error)
{
    return guarded(error, [&]()
    {
        auto doc = parseObject(data, MaxDocumentSize);
        header(doc, "mirrors", {"issued", "mirrors"});
        MirrorsDocument mirrors;
        mirrors.issued = time(doc.value("issued"), "issued");
        auto groups = list(doc.value("mirrors"), "mirrors", 1, MaxMirrors);
        QSet<QString> seen;
        for(int g = 0; g < groups.size(); g++)
        {
            auto group = list(groups[g], QString("mirrors[%1]").arg(g), 1, MaxMirrors);
            QStringList urls;
            for(int i = 0; i < group.size(); i++)
            {
                auto where = QString("mirrors[%1][%2]").arg(g).arg(i);
                auto url = string(group[i], where);
                auto host = url.mid(8).section('/', 0, 0);
                if(!matches("https://[\\x21-\\x7e]+/", url) || url.contains('@') || url.contains('?') || url.contains('#') || url.contains('\\') || host.isEmpty())
                {
                    fail(QString("%1: '%2' isn't an https:// base URL ending in / without user info, query or fragment").arg(where, url));
                }
                if(seen.contains(url))
                {
                    fail(QString("%1: '%2' is listed twice").arg(where, url));
                }
                seen.insert(url);
                urls.append(url);
            }
            mirrors.groups.append(urls);
        }
        if(seen.size() > MaxMirrors)
        {
            fail(QString("mirrors: %1 URLs, at most %2").arg(seen.size()).arg(MaxMirrors));
        }
        out = mirrors;
    });
}

bool parseBuild(const QByteArray &data, qint64 expected, BuildManifest &out, QString &error)
{
    return guarded(error, [&]()
    {
        auto doc = parseObject(data, MaxManifestSize);
        header(doc, "build", {"build", "commit", "built", "variants"});
        BuildManifest manifest;
        manifest.build = integer(doc.value("build"), "build", 1);
        if(manifest.build != expected)
        {
            fail(QString("build is %1, but the file is for build %2").arg(manifest.build).arg(expected));
        }
        manifest.commit = string(doc.value("commit"), "commit", Commit);
        manifest.built = time(doc.value("built"), "built");
        auto variants = doc.value("variants");
        if(!variants.isObject() || variants.toObject().isEmpty())
        {
            fail("variants: not an object with at least one variant");
        }
        auto variantObj = variants.toObject();
        for(auto it = variantObj.begin(); it != variantObj.end(); it++)
        {
            if(!matches(VariantId, it.key()) || it.key().size() > MaxVariantId)
            {
                fail(QString("variants: '%1' isn't a valid variant ID").arg(it.key()));
            }
            manifest.variants.insert(it.key(), variant(it.key(), it.value(), "variants." + it.key()));
        }

        // a single chain of successors from every variant: each superseded at most once, no cycles
        QMap<QString, QString> successor;
        for(auto &v: manifest.variants)
        {
            for(auto &other: v.supersedes)
            {
                if(successor.contains(other))
                {
                    fail(QString("variants: %1 is superseded by both %2 and %3").arg(other, successor.value(other), v.id));
                }
                successor.insert(other, v.id);
            }
        }
        for(auto &start: successor.keys())
        {
            QSet<QString> seen{start};
            auto current = start;
            while(successor.contains(current))
            {
                current = successor.value(current);
                if(seen.contains(current))
                {
                    fail(QString("variants: the supersedes chain from %1 loops").arg(start));
                }
                seen.insert(current);
            }
        }
        out = manifest;
    });
}

bool checkSignatureFile(const QByteArray &document, const QByteArray &signatureFile, const QStringList &trustedKeys, QString &error)
{
    return guarded(error, [&]()
    {
        if(signatureFile.size() > MaxSignatureFileSize)
        {
            fail("signature file: too large");
        }
        if(signatureFile.isEmpty() || !signatureFile.endsWith('\n'))
        {
            fail("signature file: empty, or doesn't end with a newline");
        }
        auto lines = signatureFile.left(signatureFile.size() - 1).split('\n');
        if(lines.size() > MaxSignatureLines)
        {
            fail(QString("signature file: %1 lines, allowed 1 to %2").arg(lines.size()).arg(MaxSignatureLines));
        }
        QRegularExpression lineFormat("\\A([0-9a-f]{64}) ([0-9a-f]{128})\\z");
        QStringList seen;
        int trusted = 0;
        for(int i = 0; i < lines.size(); i++)
        {
            auto m = lineFormat.match(QString::fromLatin1(lines[i]));
            if(!m.hasMatch())
            {
                fail(QString("signature file: line %1 isn't <64 hex digits> <128 hex digits>").arg(i + 1));
            }
            auto key = m.captured(1);
            if(seen.contains(key))
            {
                fail("signature file: a key is listed twice");
            }
            seen.append(key);
            if(!trustedKeys.contains(key))
            {
                continue;
            }
            trusted++;
            auto signature = QByteArray::fromHex(m.captured(2).toLatin1());
            if(!Signature::checkEd25519(document, signature, QByteArray::fromHex(key.toLatin1())))
            {
                fail(QString("the signature by %1 doesn't verify").arg(key));
            }
        }
        if(!trusted)
        {
            fail("no signature by a trusted key");
        }
    });
}

}
