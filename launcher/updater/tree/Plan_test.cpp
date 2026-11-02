#include <QTest>
#include <QTemporaryDir>

#include "TestUtil.h"
#include "TestTrees.h"
#include "FileSystem.h"
#include "updater/tree/Plan.h"

using namespace TreeUpdater;

namespace {

TagsDocument tags(qint64 stable, qint64 testing, qint64 latest)
{
    TagsDocument out;
    out.serial = 1;
    out.stable = stable;
    out.testing = testing;
    out.latest = latest;
    return out;
}

BuildManifest build(qint64 number)
{
    BuildManifest out;
    QString error;
    if(!parseBuild(TestTrees::read("main", QString("builds/%1.json").arg(number)), number, out, error))
    {
        qFatal("%s", qPrintable(error));
    }
    return out;
}

SystemRequirements::System mac(const QString &version)
{
    SystemRequirements::System system;
    system.os = "macos";
    system.osVersion = version;
    system.machineArch = "x86_64";
    system.launcherArch = "x86_64";
    system.qt = "5.6.3";
    return system;
}

void writeFile(const QString &path, const QByteArray &data)
{
    FS::ensureFilePathExists(path);
    QFile file(path);
    file.open(QFile::WriteOnly);
    file.write(data);
}

// a variant's installation under root, as its manifest lists it
void install(const Variant &variant, const QString &root)
{
    for(auto &file: variant.files)
    {
        writeFile(FS::PathCombine(root, file.path), TestTrees::read("main", contentPath(file.sha256)));
    }
}

}

class TreePlanTest : public QObject
{
    Q_OBJECT

private
slots:
    void test_tierNames()
    {
        Tier tier;
        QVERIFY(parseTier("latest", tier));
        QVERIFY(tier == Tier::Latest);
        QVERIFY(parseTier("testing", tier));
        QVERIFY(tier == Tier::Testing);
        QVERIFY(parseTier("stable", tier));
        QVERIFY(tier == Tier::Stable);
        // the old channel setting's values aren't tiers
        QVERIFY(!parseTier("develop", tier));
        QVERIFY(!parseTier("Stable", tier));
        QCOMPARE(tierName(Tier::Testing), QString("testing"));
    }

    void test_tiers_data()
    {
        QTest::addColumn<int>("tier");
        QTest::addColumn<qint64>("current");
        QTest::addColumn<bool>("switched");
        QTest::addColumn<int>("kind");
        QTest::addColumn<qint64>("build");
        // stable 4290, testing 4305, latest 4312
        QTest::newRow("stable, older installed") << int(Tier::Stable) << qint64(4280) << false << int(TierDecision::Update) << qint64(4290);
        QTest::newRow("stable, same") << int(Tier::Stable) << qint64(4290) << false << int(TierDecision::NoUpdate) << qint64(4290);
        QTest::newRow("stable, newer installed") << int(Tier::Stable) << qint64(4310) << false << int(TierDecision::BehindInstalled) << qint64(4290);
        QTest::newRow("stable, newer installed, switched") << int(Tier::Stable) << qint64(4310) << true << int(TierDecision::Downgrade) << qint64(4290);
        QTest::newRow("testing") << int(Tier::Testing) << qint64(4290) << false << int(TierDecision::Update) << qint64(4305);
        QTest::newRow("latest") << int(Tier::Latest) << qint64(4290) << false << int(TierDecision::Update) << qint64(4312);
        QTest::newRow("latest, switched, newer") << int(Tier::Latest) << qint64(4290) << true << int(TierDecision::Update) << qint64(4312);
        QTest::newRow("latest, same, switched") << int(Tier::Latest) << qint64(4312) << true << int(TierDecision::NoUpdate) << qint64(4312);
    }
    void test_tiers()
    {
        QFETCH(int, tier);
        QFETCH(qint64, current);
        QFETCH(bool, switched);
        QFETCH(int, kind);
        QFETCH(qint64, build);
        auto decision = decideTier(tags(4290, 4305, 4312), Tier(tier), current, switched);
        QCOMPARE(int(decision.kind), kind);
        QCOMPARE(decision.build, build);
    }

    // right after a promotion there is no newer Testing build: Testing is Stable
    void test_testingResolvesToStable()
    {
        auto decision = decideTier(tags(4305, 4305, 4312), Tier::Testing, 4290, false);
        QCOMPARE(decision.build, qint64(4305));
        // a Testing user ahead of the new Stable stays where they are
        decision = decideTier(tags(4305, 4305, 4312), Tier::Testing, 4310, false);
        QCOMPARE(int(decision.kind), int(TierDecision::BehindInstalled));
    }

    void test_variantFollowsSuccessors()
    {
        // 11 has macos-x86_64-qt5.15, which supersedes qt5.6, from 10.13
        auto choice = chooseVariant(build(11), "macos-x86_64-qt5.6", mac("10.15.7"));
        QVERIFY(choice.found);
        QCOMPARE(choice.variant.id, QString("macos-x86_64-qt5.15"));
        // too old for the successor: its own variant
        choice = chooseVariant(build(11), "macos-x86_64-qt5.6", mac("10.12.6"));
        QVERIFY(choice.found);
        QCOMPARE(choice.variant.id, QString("macos-x86_64-qt5.6"));
        // already on the successor
        choice = chooseVariant(build(11), "macos-x86_64-qt5.15", mac("10.15.7"));
        QVERIFY(choice.found);
        QCOMPARE(choice.variant.id, QString("macos-x86_64-qt5.15"));
        // 10 has no successor yet
        choice = chooseVariant(build(10), "macos-x86_64-qt5.6", mac("10.15.7"));
        QCOMPARE(choice.variant.id, QString("macos-x86_64-qt5.6"));
    }

    void test_variantNotForThisMachine()
    {
        // the successor isn't met and the old variant needs 10.7
        auto choice = chooseVariant(build(11), "macos-x86_64-qt5.6", mac("10.6.8"));
        QVERIFY(!choice.found);
        QVERIFY2(choice.unmet.join(" ").contains("10.7"), qPrintable(choice.unmet.join(" ")));

        // on the successor, too old for it, and the build doesn't go back
        choice = chooseVariant(build(11), "macos-x86_64-qt5.15", mac("10.12"));
        QVERIFY(!choice.found);
        QVERIFY(choice.unmet.join(" ").contains("10.13"));

        auto system = TestTrees::linuxSystem();
        system.glibc = "2.23";
        choice = chooseVariant(build(12), "linux-x86_64-qt5", system);
        QVERIFY(!choice.found);
        QVERIFY(choice.unmet.join(" ").contains("glibc 2.27"));

        choice = chooseVariant(build(12), "linux-arm64-qt5", TestTrees::linuxSystem());
        QVERIFY(!choice.found);
        QVERIFY(choice.unmet.join(" ").contains("linux-arm64-qt5"));
    }

    void test_mirrorOrder()
    {
        QList<QStringList> groups;
        groups << (QStringList() << "https://a/") << (QStringList() << "https://b/" << "https://c/");
        auto reverse = [](QStringList &list)
        {
            std::reverse(list.begin(), list.end());
        };
        auto order = mirrorOrder(groups, QStringList() << "https://c/" << "https://d/" << "https://a/", reverse);
        QCOMPARE(order, QStringList() << "https://a/" << "https://c/" << "https://b/" << "https://d/");

        order = mirrorOrder(QList<QStringList>(), QStringList() << "https://files/" << "https://r2/", reverse);
        QCOMPARE(order, QStringList() << "https://files/" << "https://r2/");

        // a group stays a group whatever the shuffle does
        for(int i = 0; i < 20; i++)
        {
            order = mirrorOrder(groups, QStringList(), randomShuffle);
            QCOMPARE(order[0], QString("https://a/"));
            QCOMPARE(QStringList(order.mid(1)).toSet(), (QStringList() << "https://b/" << "https://c/").toSet());
        }
    }

    void test_planFiles()
    {
        QTemporaryDir root;
        auto from = build(10).variants.value("linux-x86_64-qt5");
        auto to = build(11).variants.value("linux-x86_64-qt5");
        install(from, root.path());
        writeFile(root.path() + "/unrelated.txt", "the user's");

        QList<Download> downloads;
        GoUpdate::OperationList operations;
        QString error;
        QVERIFY2(planFiles(to, &from.files, root.path(), "/tmp/u", downloads, operations, error), qPrintable(error));

        // bin/libfoo.so is the same in both
        QStringList downloaded;
        for(auto &download: downloads)
        {
            downloaded.append(download.file.path);
        }
        QCOMPARE(downloaded, QStringList() << "MultiMC" << "bin/MultiMC" << "share/new.txt");
        QCOMPARE(operations.size(), 4);
        QCOMPARE(operations[0], GoUpdate::Operation::DeleteOp("share/old.txt"));
        QCOMPARE(operations[1], GoUpdate::Operation::CopyOp(downloads[0].target, "MultiMC", 0755));
        QCOMPARE(operations[2], GoUpdate::Operation::CopyOp(downloads[1].target, "bin/MultiMC", 0755));
        QCOMPARE(operations[3], GoUpdate::Operation::CopyOp(downloads[2].target, "share/new.txt", 0644));
        QVERIFY(downloads[0].target.startsWith("/tmp/u/"));
        QVERIFY(downloads[0].target != downloads[1].target);
    }

    void test_planFilesUnknownCurrent()
    {
        QTemporaryDir root;
        auto from = build(10).variants.value("linux-x86_64-qt5");
        auto to = build(11).variants.value("linux-x86_64-qt5");
        install(from, root.path());
        // a file changed locally is replaced
        writeFile(root.path() + "/bin/libfoo.so", "changed");

        QList<Download> downloads;
        GoUpdate::OperationList operations;
        QString error;
        QVERIFY(planFiles(to, nullptr, root.path(), "/tmp/u", downloads, operations, error));
        // without the installed build's list nothing is deleted
        for(auto &operation: operations)
        {
            QVERIFY(operation.type == GoUpdate::Operation::OP_REPLACE);
        }
        QCOMPARE(downloads.size(), 4);

        // up to date: nothing to do
        install(to, root.path());
        QVERIFY(planFiles(to, &to.files, root.path(), "/tmp/u", downloads, operations, error));
        QVERIFY(downloads.isEmpty());
        QVERIFY(operations.isEmpty());
    }

    void test_missingLinks()
    {
#ifdef Q_OS_WIN
        QSKIP("no symbolic links in Windows variants");
#else
        QTemporaryDir root;
        auto variant = build(11).variants.value("macos-x86_64-qt5.15");
        install(variant, root.path());
        QCOMPARE(missingLinks(variant.links, root.path()).size(), 2);

        auto framework = root.path() + "/MultiMC.app/Contents/Frameworks/QtCore.framework/";
        QVERIFY(QFile::link("5", framework + "Versions/Current"));
        QCOMPARE(missingLinks(variant.links, root.path()), QStringList() << "MultiMC.app/Contents/Frameworks/QtCore.framework/QtCore");
        // pointing at the same place another way isn't the same link
        QVERIFY(QFile::link(framework + "Versions/Current/QtCore", framework + "QtCore"));
        QCOMPARE(missingLinks(variant.links, root.path()).size(), 1);
        QFile::remove(framework + "QtCore");
        QVERIFY(QFile::link("Versions/Current/QtCore", framework + "QtCore"));
        QVERIFY(missingLinks(variant.links, root.path()).isEmpty());
#endif
    }
};

QTEST_GUILESS_MAIN(TreePlanTest)

#include "Plan_test.moc"
