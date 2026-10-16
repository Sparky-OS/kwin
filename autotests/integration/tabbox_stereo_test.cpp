/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/outputconfiguration.h"
#include "effect/effectloader.h"
#include "input.h"
#include "internalwindow.h"
#include "options.h"
#include "plugins/screenshot/screenshot.h"
#include "scene/workspacescene.h"
#include "tabbox/tabbox.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/surface.h>

#include <QDir>
#include <QWindow>

#include <linux/input.h>

namespace KWin
{

/**
 * A window switcher that fakes depth is drawn once for each eye on a stereo output: its window is
 * one eye wide on screen, twice as wide to Qt, and declared full side by side, and each item lands
 * at the depth it fakes, a rotated one per corner through its own rotation seen by each eye. On a 2D
 * output it is drawn as before. The depth is read from captures of the output: the parallax of an
 * item is the horizontal distance of its colour in the left eye's half to the same colour in the
 * right eye's half.
 *
 * The Flip Switch and Cover Switch of kdeplasma-addons are measured when their packages are found
 * (KWIN_TABBOX_STEREO_DATA names a folder that holds kwin/tabbox/flipswitch and coverswitch).
 */
class TabBoxStereoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testDrawnForEachEye();
    void testTwoDimensionalOutput();
    void testFlipSwitch();
    void testCoverSwitch();
    void testSwitchersOnTwoDimensionalOutput_data();
    void testSwitchersOnTwoDimensionalOutput();
    void testProofPictures();
};

static QSize s_outputSize(320, 240);
// the effects' own limits, at 1920 wide: popped 12 and sunk 20 pixels on a 320 wide output
static constexpr qreal s_effectPoppedLimit = 72;
static constexpr qreal s_effectSunkLimit = 120;

static OutputModeline plainMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Preferred);
}

static OutputModeline stereoMode()
{
    return OutputModeline(s_outputSize, 60000, OutputModeline::Flag::Stereo3DSideBySideFull);
}

static bool stereoOutput(bool enabled)
{
    OutputConfiguration configuration;
    configuration.changeSet(workspace()->outputs().front()->backendOutput())->currentMode = enabled ? stereoMode() : plainMode();
    workspace()->applyOutputConfiguration(configuration);
    return Test::waylandSync() && workspace()->outputs().front()->hasStereoEyes() == enabled;
}

static bool near(const QColor &pixel, const QColor &color)
{
    return std::abs(pixel.red() - color.red()) < 40 && std::abs(pixel.green() - color.green()) < 40 && std::abs(pixel.blue() - color.blue()) < 40;
}

// the first and the last x of the colour on the row at y of the half that starts at xOffset
static std::pair<int, int> span(const QImage &image, int xOffset, int y, const QColor &color)
{
    int first = -1;
    int last = -1;
    for (int x = 0; x < s_outputSize.width(); ++x) {
        if (near(image.pixelColor(xOffset + x, y), color)) {
            if (first < 0) {
                first = x;
            }
            last = x;
        }
    }
    return {first, last};
}

// the first y of the colour in the column at x of the half that starts at xOffset
static int top(const QImage &image, int xOffset, int x, const QColor &color)
{
    for (int y = 0; y < s_outputSize.height(); ++y) {
        if (near(image.pixelColor(xOffset + x, y), color)) {
            return y;
        }
    }
    return -1;
}

struct Seen
{
    int leftEdge = 0; // the parallax of the left edge, left eye minus right eye
    int rightEdge = 0;
    int centre = 0; // the x of the middle in 2D
    bool found = false;
};

static Seen seenOn(const QImage &image, int y, const QColor &color)
{
    const auto [leftFirst, leftLast] = span(image, 0, y, color);
    const auto [rightFirst, rightLast] = span(image, s_outputSize.width(), y, color);
    if (leftFirst < 0 || rightFirst < 0) {
        return Seen{};
    }
    return Seen{
        .leftEdge = leftFirst - rightFirst,
        .rightEdge = leftLast - rightLast,
        .centre = (leftFirst + leftLast + rightFirst + rightLast) / 4,
        .found = true,
    };
}

static std::optional<QImage> capture()
{
    QSignalSpy rendered(kwinApp()->scene(), &WorkspaceScene::frameRendered);
    kwinApp()->scene()->addRepaintFull();
    if (!rendered.wait()) {
        return std::nullopt;
    }
    ScreenShotManager manager;
    return manager.takeScreenShot(workspace()->outputs().front(), ScreenShotFlags(), std::nullopt);
}

static InternalWindow *switcherWindow()
{
    for (Window *window : workspace()->windows()) {
        if (auto internal = qobject_cast<InternalWindow *>(window); internal && internal->isShown()) {
            return internal;
        }
    }
    return nullptr;
}

static void setLayout(const QString &name)
{
    auto config = kwinApp()->config();
    KConfigGroup group(config, QStringLiteral("TabBox"));
    group.writeEntry("LayoutName", name);
    group.writeEntry("ShowTabBox", true);
    group.writeEntry("DelayTime", 0);
    group.writeEntry("HighlightWindows", false);
    config->sync();
    workspace()->slotReconfigure();
    options->setStereoEffectPoppedLimit(s_effectPoppedLimit);
    options->setStereoEffectSunkLimit(s_effectSunkLimit);
}

static bool hasLayout(const QString &name)
{
    return !QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("kwin/tabbox/%1/metadata.json").arg(name)).isEmpty();
}

static quint32 s_time = 0;

// Alt+Tab, holding Alt; the switcher stays up until closeSwitcher()
static InternalWindow *openSwitcher()
{
    QSignalSpy added(workspace()->tabbox(), &TabBox::TabBox::tabBoxAdded);
    Test::keyboardKeyPressed(KEY_LEFTALT, ++s_time);
    Test::keyboardKeyPressed(KEY_TAB, ++s_time);
    Test::keyboardKeyReleased(KEY_TAB, ++s_time);
    if (!added.count() && !added.wait()) {
        return nullptr;
    }
    InternalWindow *window = nullptr;
    for (int i = 0; i < 100 && !(window && window->readyForPainting()); ++i) {
        QTest::qWait(50);
        window = switcherWindow();
    }
    // the switchers' own motions settle
    QTest::qWait(1500);
    return window && window->readyForPainting() ? window : nullptr;
}

static void closeSwitcher()
{
    QSignalSpy closed(workspace()->tabbox(), &TabBox::TabBox::tabBoxClosed);
    Test::keyboardKeyReleased(KEY_LEFTALT, ++s_time);
    QTRY_VERIFY(closed.count() > 0);
    QTRY_VERIFY(!switcherWindow());
    QTRY_VERIFY(!workspace()->tabbox()->isGrabbed());
}

struct TestWindow
{
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shellSurface;
    std::unique_ptr<KWayland::Client::PlasmaShellSurface> plasmaSurface;
    Window *window = nullptr;
};

static std::optional<TestWindow> s_desktop;

static TestWindow createWindow(const QColor &color, const QSize &size)
{
    TestWindow result;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createXdgToplevelSurface(result.surface.get());
    result.window = Test::renderAndWaitForShown(result.surface.get(), size, color);
    // away from the output: the switcher shows them, nothing else does
    if (result.window) {
        result.window->move(QPoint(s_outputSize.width() + 100, 0));
    }
    return result;
}

// the desktop behind everything, as the shell draws it
static TestWindow createDesktop()
{
    TestWindow result;
    result.surface = Test::createSurface();
    result.shellSurface = Test::createXdgToplevelSurface(result.surface.get());
    result.plasmaSurface.reset(Test::waylandPlasmaShell()->createSurface(result.surface.get()));
    result.plasmaSurface->setRole(KWayland::Client::PlasmaShellSurface::Role::Desktop);
    result.window = Test::renderAndWaitForShown(result.surface.get(), s_outputSize, QColor(48, 48, 48));
    return result;
}

void TabBoxStereoTest::initTestCase()
{
    if (!Test::renderNodeAvailable()) {
        QSKIP("A DRM render node is required for the virtual EGL screenshot backend");
    }
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    qputenv("KWIN_XKB_DEFAULT_KEYMAP", "1");
    QStringList dataDirs{QFINDTESTDATA("data/tabbox")};
    if (const QString extra = qEnvironmentVariable("KWIN_TABBOX_STEREO_DATA"); !extra.isEmpty()) {
        dataDirs.prepend(extra);
    }
    dataDirs.append(qEnvironmentVariable("XDG_DATA_DIRS", QStringLiteral("/usr/local/share:/usr/share")).split(QLatin1Char(':')));
    qputenv("XDG_DATA_DIRS", dataDirs.join(QLatin1Char(':')).toUtf8());
    QVERIFY(waylandServer()->init(qAppName()));

    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    const auto builtinNames = EffectLoader().listOfKnownEffects();
    for (const QString &name : builtinNames) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    KConfigGroup depth(config, QStringLiteral("StereoDepth"));
    depth.writeEntry("SunkLimit", 0);
    depth.writeEntry("PoppedLimit", 0);
    depth.writeEntry("EffectSunkLimit", s_effectSunkLimit);
    depth.writeEntry("EffectPoppedLimit", s_effectPoppedLimit);
    KConfigGroup(config, QStringLiteral("KDE")).writeEntry("AnimationDurationFactor", 0);
    config->sync();
    kwinApp()->setConfig(config);

    kwinApp()->start();
}

void TabBoxStereoTest::init()
{
    const bool large = QByteArray(QTest::currentTestFunction()).contains("Switch");
    const bool proof = qstrcmp(QTest::currentTestFunction(), "testProofPictures") == 0;
    s_outputSize = proof ? QSize(1920, 1080) : (large ? QSize(640, 360) : QSize(320, 240));
    Test::setOutputConfig({Test::OutputInfo{
        .geometry = Rect(QPoint(), s_outputSize),
        .modes = {plainMode(), stereoMode()},
    }});
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat | Test::AdditionalWaylandInterface::PlasmaShell));
    QVERIFY(Test::waitForWaylandKeyboard());
    QVERIFY(stereoOutput(true));
    s_desktop = createDesktop();
    QVERIFY(s_desktop->window && s_desktop->window->isDesktop());
}

void TabBoxStereoTest::cleanup()
{
    if (switcherWindow()) {
        closeSwitcher();
    }
    s_desktop.reset();
    Test::destroyWaylandConnection();
    QTRY_VERIFY(workspace()->windows().isEmpty());
}

void TabBoxStereoTest::testDrawnForEachEye()
{
    setLayout(QStringLiteral("stereotest"));
    auto a = createWindow(Qt::darkGray, QSize(60, 40));
    auto b = createWindow(Qt::darkGray, QSize(60, 40));
    QVERIFY(a.window && b.window);

    InternalWindow *window = openSwitcher();
    QVERIFY(window);
    // one eye wide on screen, both eyes side by side to Qt, declared
    QCOMPARE(window->frameGeometry(), RectF(QPointF(0, 0), s_outputSize));
    QCOMPARE(window->handle()->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
    QCOMPARE(window->stereoContent(), StereoContentSideBySideFull);
    QVERIFY(window->holdsStereoContent());

    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), QSize(s_outputSize.width() * 2, s_outputSize.height()));
    const QString directory = qEnvironmentVariable("KWIN_TABBOX_STEREO_PROOF");
    if (!directory.isEmpty()) {
        QVERIFY(image->save(directory + QStringLiteral("/stereotest.png")));
    }

    // popped and sunk at the effects' limits
    const Seen popped = seenOn(*image, 40, Qt::red);
    const Seen sunk = seenOn(*image, 40, QColor(0, 255, 0));
    qInfo() << "measured popped" << popped.leftEdge << "sunk" << sunk.leftEdge;
    QCOMPARE(popped.leftEdge, 12);
    QCOMPARE(sunk.leftEdge, -20);

    // the card turned 50 degrees about its middle, 10 pixels behind the screen there: its left edge
    // comes nearer and its right edge goes back, by 0.1 pixels of parallax per pixel of the depth the
    // rotation gives it, seen through the rotation's own camera 1024 pixels away
    const Seen card = seenOn(*image, 50, Qt::blue);
    QVERIFY(card.found);
    for (const qreal edge : {-60.0, 60.0}) {
        const qreal angle = qDegreesToRadians(50.0);
        const qreal depth = -std::sin(angle) * edge;
        const qreal w = 1 - depth / 1024;
        const qreal parallax = -10 + 0.1 * depth / w;
        const int measured = edge < 0 ? card.leftEdge : card.rightEdge;
        qInfo() << "measured card edge" << edge << measured << "expected" << parallax;
        QVERIFY(std::abs(measured - parallax) <= 1.5);
    }
    QVERIFY(card.leftEdge > card.rightEdge);

    // inside a tilted view, the yellow square's middle at the popped limit
    const Seen inner = seenOn(*image, 140, Qt::yellow);
    QVERIFY(inner.found);
    qInfo() << "measured inside the tilted view" << inner.leftEdge << inner.rightEdge;
    QVERIFY(std::abs((inner.leftEdge + inner.rightEdge) / 2.0 - 12) <= 1);

    // no vertical parallax, and the strip at the screen identical in both eyes
    QCOMPARE(top(*image, 0, 40, Qt::red), top(*image, s_outputSize.width(), 52, Qt::red));
    QVERIFY(std::abs(top(*image, 0, inner.centre, Qt::yellow) - top(*image, s_outputSize.width(), inner.centre, Qt::yellow)) <= 1);
    const QRect strip(20, 200, 280, 20);
    QCOMPARE(image->copy(strip), image->copy(strip.translated(s_outputSize.width(), 0)));

    closeSwitcher();
}

void TabBoxStereoTest::testTwoDimensionalOutput()
{
    QVERIFY(stereoOutput(false));
    setLayout(QStringLiteral("stereotest"));
    auto a = createWindow(Qt::darkGray, QSize(60, 40));
    auto b = createWindow(Qt::darkGray, QSize(60, 40));
    QVERIFY(a.window && b.window);

    InternalWindow *window = openSwitcher();
    QVERIFY(window);
    QCOMPARE(window->frameGeometry(), RectF(QPointF(0, 0), s_outputSize));
    QCOMPARE(window->handle()->size(), s_outputSize);
    QCOMPARE(window->stereoContent(), StereoContentNone);

    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), s_outputSize);
    // everything at its 2D place
    QCOMPARE(span(*image, 0, 40, Qt::red).first, 20);
    QCOMPARE(span(*image, 0, 40, QColor(0, 255, 0)).first, 80);
    closeSwitcher();
}

static std::vector<TestWindow> createColouredWindows()
{
    std::vector<TestWindow> windows;
    for (const QColor &color : {QColor(Qt::red), QColor(0, 255, 0), QColor(Qt::blue), QColor(Qt::yellow)}) {
        windows.push_back(createWindow(color, QSize(160, 120)));
    }
    return windows;
}

void TabBoxStereoTest::testFlipSwitch()
{
    if (!hasLayout(QStringLiteral("flipswitch"))) {
        QSKIP("The Flip Switch is not installed");
    }
    setLayout(QStringLiteral("flipswitch"));
    const std::vector<TestWindow> windows = createColouredWindows();
    for (const TestWindow &window : windows) {
        QVERIFY(window.window);
    }
    InternalWindow *switcher = openSwitcher();
    QVERIFY(switcher);
    QCOMPARE(switcher->stereoContent(), StereoContentSideBySideFull);
    const auto image = capture();
    QVERIFY(image);
    const QString directory = qEnvironmentVariable("KWIN_TABBOX_STEREO_PROOF");
    if (!directory.isEmpty()) {
        QVERIFY(image->save(directory + QStringLiteral("/flipswitch.png")));
    }

    // the effects' limits on a 640 wide output
    const qreal popped = s_effectPoppedLimit * s_outputSize.width() / 1920;
    const qreal sunk = s_effectSunkLimit * s_outputSize.width() / 1920;
    // each card on the row through its middle: the middle's parallax, from its two edges
    struct Card
    {
        QColor color;
        Seen seen;
        int row;
    };
    QList<Card> cards;
    for (const QColor &color : {QColor(Qt::red), QColor(0, 255, 0), QColor(Qt::blue), QColor(Qt::yellow)}) {
        // a few rows below the card's top: the nearer cards lie below and to the right, so there
        // both of its edges are seen
        int row = -1;
        for (int y = 0; y < s_outputSize.height() && row < 0; ++y) {
            if (span(*image, 0, y, color).first >= 0) {
                row = y + 6;
            }
        }
        QVERIFY(row >= 0);
        cards.append(Card{color, seenOn(*image, row, color), row});
    }
    for (const Card &card : cards) {
        const qreal middle = (card.seen.leftEdge + card.seen.rightEdge) / 2.0;
        qInfo() << "measured flip card" << card.color.name() << "row" << card.row << "left edge" << card.seen.leftEdge << "right edge" << card.seen.rightEdge << "middle" << middle;
        // nothing beyond the limits, the right side of every card further back: the view is tilted
        QVERIFY(middle <= popped + 1);
        QVERIFY(middle >= -sunk - 1);
        QVERIFY(card.seen.rightEdge <= card.seen.leftEdge);
    }
    // the current window's card is the nearest, at the popped limit in its middle
    const auto nearest = std::ranges::max_element(cards, {}, [](const Card &card) {
        return card.seen.leftEdge + card.seen.rightEdge;
    });
    const qreal currentParallax = (nearest->seen.leftEdge + nearest->seen.rightEdge) / 2.0;
    qInfo() << "measured nearest flip card" << nearest->color.name() << currentParallax << "expected" << popped;
    QVERIFY(std::abs(currentParallax - popped) <= 1.5);
    // four windows lie a quarter of the path apart, the current one at a fifth from its near end, so
    // at 0.8, 0.55, 0.3 and 0.05 of the way back from the far end; the far end is sunk and the
    // current window popped, in a straight line between them
    QList<qreal> middles;
    for (const Card &card : cards) {
        middles.append((card.seen.leftEdge + card.seen.rightEdge) / 2.0);
    }
    std::ranges::sort(middles, std::greater());
    const QList<qreal> places{0.8, 0.55, 0.3, 0.05};
    for (int i = 0; i < 4; ++i) {
        const qreal expected = (popped + sunk) * places[i] / 0.8 - sunk;
        qInfo() << "measured flip card" << i << middles[i] << "expected" << expected;
        QVERIFY(std::abs(middles[i] - expected) <= 1.5);
    }
    // no vertical parallax at each card's middle
    for (const Card &card : cards) {
        QVERIFY(std::abs(top(*image, 0, card.seen.centre, card.color) - top(*image, s_outputSize.width(), card.seen.centre, card.color)) <= 1);
    }
    closeSwitcher();
}

void TabBoxStereoTest::testCoverSwitch()
{
    if (!hasLayout(QStringLiteral("coverswitch"))) {
        QSKIP("The Cover Switch is not installed");
    }
    setLayout(QStringLiteral("coverswitch"));
    const std::vector<TestWindow> windows = createColouredWindows();
    for (const TestWindow &window : windows) {
        QVERIFY(window.window);
    }
    InternalWindow *switcher = openSwitcher();
    QVERIFY(switcher);
    QCOMPARE(switcher->stereoContent(), StereoContentSideBySideFull);
    const auto image = capture();
    QVERIFY(image);
    const QString directory = qEnvironmentVariable("KWIN_TABBOX_STEREO_PROOF");
    if (!directory.isEmpty()) {
        QVERIFY(image->save(directory + QStringLiteral("/coverswitch.png")));
    }

    const qreal sunk = s_effectSunkLimit * s_outputSize.width() / 1920;
    int atScreen = 0;
    QList<qreal> middles;
    for (const QColor &color : {QColor(Qt::red), QColor(0, 255, 0), QColor(Qt::blue), QColor(Qt::yellow)}) {
        int bestRow = -1;
        int bestWidth = 0;
        for (int y = 0; y < s_outputSize.height(); ++y) {
            const auto [first, last] = span(*image, 0, y, color);
            if (first >= 0 && last - first > bestWidth) {
                bestWidth = last - first;
                bestRow = y;
            }
        }
        QVERIFY(bestRow >= 0);
        const Seen seen = seenOn(*image, bestRow, color);
        qInfo() << "measured cover" << color.name() << "row" << bestRow << "x" << seen.centre << "left edge" << seen.leftEdge << "right edge" << seen.rightEdge;
        middles.append((seen.leftEdge + seen.rightEdge) / 2.0);
        // the row recedes behind the screen, never in front of it
        QVERIFY(seen.leftEdge <= 1 && seen.rightEdge <= 1);
        QVERIFY(seen.leftEdge >= -sunk - 2 && seen.rightEdge >= -sunk - 2);
        if (std::abs(seen.leftEdge) <= 1 && std::abs(seen.rightEdge) <= 1) {
            atScreen++;
        } else if (seen.centre < s_outputSize.width() / 2) {
            // a cover on the left is turned about its vertical axis: its outer edge nearer
            QVERIFY(seen.leftEdge > seen.rightEdge);
        } else {
            QVERIFY(seen.rightEdge > seen.leftEdge);
        }
    }
    // the current cover at the screen
    QCOMPARE(atScreen, 1);
    // four covers a quarter of the path apart, the current one at 0.49: the others at 0.74, 0.99
    // and 0.24 of the path, which the switcher's path puts at 0.52, 0.02 and 0.48 of the way from
    // the row's ends to the current cover; the ends are sunk
    std::ranges::sort(middles, std::greater());
    const QList<qreal> places{1, 0.52, 0.48, 0.02};
    for (int i = 0; i < 4; ++i) {
        const qreal expected = -sunk * (1 - places[i]);
        qInfo() << "measured cover" << i << middles[i] << "expected" << expected;
        QVERIFY(std::abs(middles[i] - expected) <= 1.5);
    }
    closeSwitcher();
}

void TabBoxStereoTest::testSwitchersOnTwoDimensionalOutput_data()
{
    QTest::addColumn<QString>("layout");
    QTest::newRow("flipswitch") << QStringLiteral("flipswitch");
    QTest::newRow("coverswitch") << QStringLiteral("coverswitch");
}

void TabBoxStereoTest::testSwitchersOnTwoDimensionalOutput()
{
    QFETCH(QString, layout);
    if (!hasLayout(layout)) {
        QSKIP("The switcher is not installed");
    }
    QVERIFY(stereoOutput(false));
    setLayout(layout);
    const std::vector<TestWindow> windows = createColouredWindows();
    for (const TestWindow &window : windows) {
        QVERIFY(window.window);
    }
    InternalWindow *switcher = openSwitcher();
    QVERIFY(switcher);
    QCOMPARE(switcher->stereoContent(), StereoContentNone);
    QCOMPARE(switcher->handle()->size(), s_outputSize);
    const auto image = capture();
    QVERIFY(image);
    QCOMPARE(image->size(), s_outputSize);
    // compared with the stock switcher's capture by the caller
    const QString directory = qEnvironmentVariable("KWIN_TABBOX_STEREO_PROOF");
    if (!directory.isEmpty()) {
        QVERIFY(image->save(directory + QStringLiteral("/%1-2d.png").arg(layout)));
    }
    closeSwitcher();
}

void TabBoxStereoTest::testProofPictures()
{
    // both switchers on a 1920 wide stereo output at the effects' default limits, for the eye
    const QString directory = qEnvironmentVariable("KWIN_TABBOX_STEREO_PROOF");
    if (directory.isEmpty() || !hasLayout(QStringLiteral("flipswitch")) || !hasLayout(QStringLiteral("coverswitch"))) {
        QSKIP("Proof pictures are taken on request, with both switchers installed");
    }
    std::vector<TestWindow> windows;
    for (const QColor &color : {QColor(200, 40, 40), QColor(40, 160, 60), QColor(40, 80, 200), QColor(220, 180, 40), QColor(140, 60, 180)}) {
        windows.push_back(createWindow(color, QSize(960, 600)));
        QVERIFY(windows.back().window);
    }
    for (const QString &layout : {QStringLiteral("flipswitch"), QStringLiteral("coverswitch")}) {
        setLayout(layout);
        options->setStereoEffectPoppedLimit(12);
        options->setStereoEffectSunkLimit(30);
        InternalWindow *switcher = openSwitcher();
        QVERIFY(switcher);
        QCOMPARE(switcher->stereoContent(), StereoContentSideBySideFull);
        const auto image = capture();
        QVERIFY(image);
        QCOMPARE(image->size(), QSize(3840, 1080));
        QVERIFY(image->save(directory + QStringLiteral("/%1-1920-defaults.png").arg(layout)));
        closeSwitcher();
    }
}

}

WAYLANDTEST_MAIN(KWin::TabBoxStereoTest)
#include "tabbox_stereo_test.moc"
