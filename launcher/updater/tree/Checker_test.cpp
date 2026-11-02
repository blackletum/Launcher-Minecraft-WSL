#include <QTest>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include "TestUtil.h"
#include "TestTrees.h"
#include "updater/tree/Checker.h"
#include "updater/tree/DownloadTask.h"
#include "FileSystem.h"

using namespace TreeUpdater;

class TreeCheckerTest : public QObject
{
    Q_OBJECT

    shared_qobject_ptr<QNetworkAccessManager> m_network;
    std::unique_ptr<QTemporaryDir> m_dir;

    // a Launcher on build 10 of linux-x86_64-qt5, starting from the trees' floor, with `trees` compiled in
    Config config(const QStringList &trees, qint64 currentBuild = 10)
    {
        Config out;
        for(auto &tree: trees)
        {
            out.mirrors.append(TestTrees::url(tree));
        }
        out.rootKeys = TestTrees::rootKeys();
        out.floorKeys = TestTrees::floor();
        out.variant = "linux-x86_64-qt5";
        out.currentBuild = currentBuild;
        out.statePath = m_dir->path() + "/updater/state.json";
        out.root = m_dir->path() + "/root";
        out.system = TestTrees::linuxSystem();
        out.now = QDateTime(QDate(2026, 11, 1), QTime(12, 0), Qt::UTC);
        out.shuffle = [](QStringList &) {};
        return out;
    }

    CheckResult run(const Config &config, Tier tier, bool tierSwitched = false)
    {
        Checker checker(m_network, config);
        CheckResult out;
        bool done = false;
        connect(&checker, &Checker::finished, [&](const CheckResult &result)
        {
            out = result;
            done = true;
        });
        checker.check(tier, tierSwitched, true);
        QElapsedTimer timer;
        timer.start();
        while(!done && timer.elapsed() < 20000)
        {
            QTest::qWait(10);
        }
        if(!done)
        {
            qWarning() << "the check didn't finish";
        }
        return out;
    }

private
slots:
    void init()
    {
        m_network.reset(new QNetworkAccessManager());
        m_dir.reset(new QTemporaryDir());
    }

    void test_noUpdate()
    {
        auto result = run(config({"main"}), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::NoUpdate));
        QCOMPARE(result.build, qint64(10));
        QVERIFY(result.notify);
        QVERIFY(!result.currentWithdrawn);

        // what was verified is kept
        State state(TestTrees::floor(), TestTrees::rootKeys(), 0);
        state.load(m_dir->path() + "/updater/state.json");
        QCOMPARE(state.serial(), qint64(7));
        QCOMPARE(state.keys().version, qint64(2));
        QVERIFY(state.hasMirrors());
    }

    void test_update_data()
    {
        QTest::addColumn<int>("tier");
        QTest::addColumn<qint64>("build");
        QTest::newRow("latest") << int(Tier::Latest) << qint64(12);
        QTest::newRow("testing") << int(Tier::Testing) << qint64(11);
    }
    void test_update()
    {
        QFETCH(int, tier);
        QFETCH(qint64, build);
        auto result = run(config({"main"}), Tier(tier));
        QCOMPARE(int(result.kind), int(CheckResult::Update));
        QCOMPARE(result.build, build);
        QCOMPARE(result.variant.id, QString("linux-x86_64-qt5"));
        QCOMPARE(result.commit, QString("%1").arg(0xc0ffee + build, 40, 16, QChar('0')));
        // build 10's list, for what to delete
        QVERIFY(result.haveCurrentFiles);
        QCOMPARE(result.currentFiles.last().path, QString("share/old.txt"));
        QCOMPARE(result.mirrors.first(), TestTrees::url("main"));
    }

    void test_behindAndDowngrade()
    {
        auto result = run(config({"main"}, 11), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::BehindInstalled));
        QCOMPARE(result.build, qint64(10));

        result = run(config({"main"}, 11), Tier::Stable, true);
        QCOMPARE(int(result.kind), int(CheckResult::Downgrade));
        QCOMPARE(result.build, qint64(10));
        QVERIFY(result.tierSwitched);
        QCOMPARE(result.currentFiles.last().path, QString("share/new.txt"));
    }

    void test_withdrawnBuild()
    {
        // build 9 is withdrawn and its manifest is gone: nothing to delete is known
        auto result = run(config({"main"}, 9), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::Update));
        QVERIFY(result.currentWithdrawn);
        QVERIFY(!result.haveCurrentFiles);
        QVERIFY(result.messages.join(" ").contains("withdrawn"));
    }

    void test_notForThisMachine()
    {
        auto cfg = config({"main"});
        cfg.system.glibc = "2.23";
        auto result = run(cfg, Tier::Latest);
        QCOMPARE(int(result.kind), int(CheckResult::NotForThisMachine));
        QCOMPARE(result.build, qint64(12));
        QVERIFY(result.messages.join(" ").contains("glibc 2.27"));
        // Testing still works there
        QCOMPARE(int(run(cfg, Tier::Testing).kind), int(CheckResult::Update));
    }

    void test_linksMustBeThere()
    {
        auto cfg = config({"main"});
        cfg.variant = "macos-x86_64-qt5.6";
        cfg.system.os = "macos";
        cfg.system.osVersion = "10.15.7";
        auto result = run(cfg, Tier::Testing);
        QCOMPARE(int(result.kind), int(CheckResult::CannotApply));
        QCOMPARE(result.variant.id, QString("macos-x86_64-qt5.15"));
        QVERIFY(result.messages.join(" ").contains("symbolic links"));
    }

    // the first mirror's tags don't verify: the whole check goes to the next one
    void test_fallback()
    {
        auto result = run(config({"broken", "main"}), Tier::Latest);
        QCOMPARE(int(result.kind), int(CheckResult::Update));
        QCOMPARE(result.build, qint64(12));
        QCOMPARE(result.mirrors.first(), TestTrees::url("main"));
        QCOMPARE(result.mirrors.last(), TestTrees::url("broken"));

        // not there at all
        result = run(config({"nowhere", "main"}), Tier::Latest);
        QCOMPARE(int(result.kind), int(CheckResult::Update));
    }

    void test_allMirrorsFail()
    {
        auto result = run(config({"broken"}), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::Failed));
        QVERIFY2(result.messages.join(" ").contains("tags.json: the signature by"), qPrintable(result.messages.join(" ")));
    }

    void test_mirrorListIsUsed()
    {
        run(config({"main"}), Tier::Stable);
        // the stored list comes first (.invalid hosts, which fail), the compiled-in hosts after it
        auto result = run(config({"main"}), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::NoUpdate));
        QCOMPARE(result.mirrors.first(), TestTrees::url("main"));
        QCOMPARE(result.mirrors.size(), 4);
        QVERIFY(result.mirrors.contains("https://files.invalid/updater/"));
    }

    // a mirror that lags behind serves an older (validly signed) tags document
    void test_lowerSerialRefused()
    {
        QCOMPARE(int(run(config({"stale"}), Tier::Stable).kind), int(CheckResult::NoUpdate));
        QCOMPARE(int(run(config({"main"}), Tier::Stable).kind), int(CheckResult::NoUpdate));
        // now serial 7 and keys version 2 are known
        auto result = run(config({"stale", "main"}), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::NoUpdate));
        QCOMPARE(result.mirrors.first(), TestTrees::url("main"));

        result = run(config({"stale"}), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::Failed));
        // stale's documents are signed by online1, which keys version 2 dropped
        QVERIFY2(result.messages.join(" ").contains("no signature by a trusted key"), qPrintable(result.messages.join(" ")));
    }

    void test_serialFloor()
    {
        auto cfg = config({"main"});
        cfg.serialFloor = 8;
        auto result = run(cfg, Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::Failed));
        QVERIFY(result.messages.join(" ").contains("serial 7 is lower than 8"));
    }

    void test_rootRotation()
    {
        auto result = run(config({"rotated"}), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::NoUpdate));
        State state(TestTrees::floor(), TestTrees::rootKeys(), 0);
        state.load(m_dir->path() + "/updater/state.json");
        QCOMPARE(state.keys().version, qint64(3));
        QCOMPARE(state.serial(), qint64(8));

        // from keys version 3, main's documents (signed by online2) aren't trusted
        result = run(config({"main"}), Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::Failed));
    }

    void test_expiredKeys()
    {
        auto cfg = config({"main"});
        cfg.now = QDateTime(QDate(2028, 11, 1), QTime(0, 0), Qt::UTC);
        auto result = run(cfg, Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::Failed));
        QVERIFY(result.messages.join(" ").contains("expired"));

        // the intermediate version 1 expired already, which doesn't matter
        cfg.now = QDateTime(QDate(2027, 6, 1), QTime(0, 0), Qt::UTC);
        QCOMPARE(int(run(cfg, Tier::Stable).kind), int(CheckResult::NoUpdate));
    }

    void test_wrongPinnedKeys()
    {
        auto cfg = config({"main"});
        cfg.rootKeys = QStringList() << QString(64, 'a');
        Checker checker(m_network, cfg);
        QVERIFY(!checker.configError().isEmpty());
        auto result = run(cfg, Tier::Stable);
        QCOMPARE(int(result.kind), int(CheckResult::Failed));
        QVERIFY(result.messages.join(" ").contains("pinned root keys"));
    }

    void test_queuedCheck()
    {
        Checker checker(m_network, config({"main"}, 11));
        QList<CheckResult> results;
        connect(&checker, &Checker::finished, [&](const CheckResult &result) { results.append(result); });
        checker.check(Tier::Stable, false, false);
        checker.check(Tier::Stable, true, true);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 20000);
        QCOMPARE(int(results[0].kind), int(CheckResult::BehindInstalled));
        QCOMPARE(int(results[1].kind), int(CheckResult::Downgrade));
        QVERIFY(results[1].notify);
    }

    /*
     * A real tree, only when asked for (it needs the network): check from build 1 on this
     * machine's own system, then download everything into an empty installation.
     *   TREE_UPDATER_LIVE_MIRRORS  base URLs, separated by ;
     *   TREE_UPDATER_LIVE_FLOOR    the floor keys document (its root keys are pinned)
     *   TREE_UPDATER_LIVE_VARIANT  the variant ID to check for
     */
    void test_liveTree()
    {
        auto mirrors = QString::fromLocal8Bit(qgetenv("TREE_UPDATER_LIVE_MIRRORS")).split(';', QString::SkipEmptyParts);
        if(mirrors.isEmpty())
        {
            QSKIP("TREE_UPDATER_LIVE_MIRRORS isn't set");
        }
        QFile floorFile(QString::fromLocal8Bit(qgetenv("TREE_UPDATER_LIVE_FLOOR")));
        QVERIFY(floorFile.open(QFile::ReadOnly));
        auto floor = floorFile.readAll();

        Config cfg;
        cfg.mirrors = mirrors;
        for(auto key: QJsonDocument::fromJson(floor).object().value("root_keys").toArray())
        {
            cfg.rootKeys.append(key.toString());
        }
        cfg.floorKeys = floor;
        cfg.variant = QString::fromLocal8Bit(qgetenv("TREE_UPDATER_LIVE_VARIANT"));
        cfg.currentBuild = 1;
        cfg.statePath = m_dir->path() + "/updater/state.json";
        cfg.root = m_dir->path() + "/root";
        cfg.system = SystemRequirements::detect();
        auto result = run(cfg, Tier::Latest);
        qDebug() << "live tree:" << result.kind << "build" << result.build << "variant" << result.variant.id << result.messages;
        QCOMPARE(int(result.kind), int(CheckResult::Update));
        QVERIFY(!result.haveCurrentFiles);

        auto target = m_dir->path() + "/update/XXXXXX";
        FS::ensureFilePathExists(target);
        TreeUpdater::DownloadTask task(m_network, result, cfg.root, target);
        bool done = false, succeeded = false;
        connect(&task, &Task::succeeded, [&]() { done = succeeded = true; });
        connect(&task, &Task::failed, [&](QString) { done = true; });
        task.start();
        QTRY_VERIFY_WITH_TIMEOUT(done, 300000);
        QVERIFY2(succeeded, qPrintable(task.failReason()));
        QCOMPARE(task.operations().size(), result.variant.files.size());
        qDebug() << "live tree: downloaded" << result.variant.files.size() << "files";
    }
};

QTEST_GUILESS_MAIN(TreeCheckerTest)

#include "Checker_test.moc"
