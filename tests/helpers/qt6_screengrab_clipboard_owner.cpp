// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only Qt6 clipboard owner modeled on LXQt ScreenGrab::copyScreen().
// This program MUST be launched only by an operator-attested private Xvfb
// harness. It never captures a screen or reads an existing clipboard.
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QFileInfo>
#include <QPixmap>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace
{
struct SyntheticFixture
{
    const char *sha256;
    qsizetype bytes;
    int width;
    int height;
};

// These are the existing deterministic fixtures from
// tests/firefox_chansrv_consumer.py; never accept a user screenshot.
constexpr std::array<SyntheticFixture, 4> approvedFixtures{{
    {"c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c", 1049471, 512, 512},
    {"d9b7864e95e934ee999ee333ce9bf86adcf823aaca271634bafb8d8b9d3f6c22", 2401598, 1000, 800},
    {"cca28eec0cce17ae047221aa3177ed1765ad6d5884b7dc60df2ce3a3ff3a7cf4", 2286451, 1000, 760},
    {"ba8246c60e667f7cf553d6369887e7c976d58529faad60977f6681635d07e106", 3241953, 1200, 900},
}};

int fail(const char *reason)
{
    std::fprintf(stderr, "qt6-screengrab-owner: %s\n", reason);
    return 2;
}

bool isInsideRoot(const QString &canonicalRoot, const QString &canonicalPath)
{
    return !canonicalRoot.isEmpty()
        && canonicalPath.startsWith(canonicalRoot + QDir::separator());
}
} // namespace

int main(int argc, char *argv[])
{
    // Never initialize Qt/X11 before validating the isolated display and
    // caller-supplied test paths. The live :0 session is always rejected.
    // Default: ScreenGrab setPixmap(). Alternative: Qt owns only the
    // original allowlisted PNG bytes. This isolates Qt's image MIME
    // expansion from chansrv's delayed remote CLIPRDR rendering.
    const bool controlled = argc >= 3 &&
        std::strcmp(argv[1], "--controlled") == 0;
    const int modeIndex = controlled ? 2 : 1;
    const bool pngOnly = argc > modeIndex + 1 &&
        std::strcmp(argv[modeIndex], "--png-only") == 0;
    if (argc != modeIndex + (pngOnly ? 2 : 1))
    {
        return fail("expected [--controlled] [--png-only] <approved synthetic PNG>");
    }

    const QByteArray display = qgetenv("DISPLAY");
    bool validDisplayNumber = false;
    const int displayNumber = display.startsWith(':')
        ? display.mid(1).toInt(&validDisplayNumber) : -1;
    if (!validDisplayNumber || displayNumber < 191 || displayNumber > 249)
    {
        return fail("DISPLAY is not in the private Xvfb test range");
    }

    if (qEnvironmentVariable("QT_QPA_PLATFORM") != QStringLiteral("xcb")
        || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
    {
        return fail("test must use isolated XCB backend without Wayland");
    }

    const QFileInfo releaseDirectory(qEnvironmentVariable("XRDP_CONSOLE_RELEASE_ROOT"));
    if (!releaseDirectory.isDir() || releaseDirectory.isSymLink()
        || releaseDirectory.fileName() != QStringLiteral(".release")
        || releaseDirectory.ownerId() != geteuid())
    {
        return fail("missing safe, owned, existing .release root");
    }

    const QString canonicalRoot = releaseDirectory.canonicalFilePath();
    if (canonicalRoot.isEmpty())
    {
        return fail("cannot canonicalize release root");
    }

    const QFileInfo authority(qEnvironmentVariable("XAUTHORITY"));
    const QString authorityPath = authority.canonicalFilePath();
    if (!authority.isFile() || authority.isSymLink() || authority.size() == 0
        || authority.ownerId() != geteuid()
        || !isInsideRoot(canonicalRoot, authorityPath))
    {
        return fail("XAUTHORITY is not a private release-root file");
    }

    const QFileInfo inputFile(QString::fromLocal8Bit(argv[argc - 1]));
    const QString inputPath = inputFile.canonicalFilePath();
    if (!inputFile.isFile() || inputFile.size() < 1
        || inputFile.size() > 8 * 1024 * 1024
        || !isInsideRoot(canonicalRoot, inputPath))
    {
        return fail("synthetic PNG is outside the approved release workspace");
    }

    QFile fixtureFile(inputPath);
    if (!fixtureFile.open(QIODevice::ReadOnly))
    {
        return fail("cannot open synthetic fixture");
    }
    const QByteArray encodedImage = fixtureFile.readAll();
    if (encodedImage.size() != inputFile.size())
    {
        return fail("incomplete synthetic fixture read");
    }
    const QByteArray digest = QCryptographicHash::hash(
        encodedImage, QCryptographicHash::Sha256).toHex();

    const auto matchingFixture = std::ranges::find_if(
        approvedFixtures, [&](const SyntheticFixture &fixture) {
            return encodedImage.size() == fixture.bytes && digest == fixture.sha256;
        });
    if (matchingFixture == approvedFixtures.end())
    {
        return fail("fixture checksum or length is not allowlisted");
    }

    QApplication application(argc, argv);
    QPixmap image;
    if (!image.loadFromData(encodedImage, "PNG")
        || image.width() != matchingFixture->width
        || image.height() != matchingFixture->height)
    {
        return fail("fixture PNG decode or dimensions mismatch");
    }

    QClipboard *clipboard = QApplication::clipboard();
    if (clipboard == nullptr)
    {
        return fail("Qt clipboard unavailable");
    }

    // No image data transfer or clipboard operation occurs on a worker
    // thread. QClipboard owns the MIME object for this bounded event loop.
    if (pngOnly)
    {
        // A CONTROL, not a claim that ScreenGrab uses this API. Unlike
        // setPixmap(), the Qt owner starts with only encoded image/png.
        auto *mime = new QMimeData;
        mime->setData(QStringLiteral("image/png"), encodedImage);
        clipboard->setMimeData(mime, QClipboard::Clipboard);
    }
    else
    {
        // The exact clipboard API used by LXQt ScreenGrab.
        clipboard->setPixmap(image, QClipboard::Clipboard);
    }
    if (!clipboard->ownsClipboard())
    {
        return fail("Qt failed to acquire private CLIPBOARD selection");
    }

    // Controlled mode fixes the old 45-second race: the future owner
    // controller must receive READY after ownership, then send 'q' on the
    // held pipe (or close it) after a trusted paste receipt. A separate
    // 180-second hard ceiling bounds hangs and abandoned callers.
    // The old direct invocation retains its original 45-second behavior.
    QSocketNotifier controlInput(STDIN_FILENO, QSocketNotifier::Read, &application);
    controlInput.setEnabled(controlled);
    if (controlled)
    {
        QObject::connect(
            &controlInput,
            QOverload<QSocketDescriptor, QSocketNotifier::Type>::of(
                &QSocketNotifier::activated),
            &application,
            [](QSocketDescriptor, QSocketNotifier::Type) {
            char command = 0;
            const ssize_t got = ::read(STDIN_FILENO, &command, 1);
            // Unknown command, EOF or I/O failure all terminate the
            // isolated test owner; never leave a clipboard owner behind.
            if (got != 1 || command != 'q')
            {
                std::fprintf(stderr, "qt6-screengrab-owner: invalid control or EOF\n");
            }
            QCoreApplication::quit();
        });
        // Deliberately emit only a state marker, never image bytes.
        std::puts("XRDP_CONSOLE_QT_OWNER_READY");
        std::fflush(stdout);
    }

    QTimer::singleShot(controlled ? 180'000 : 45'000, &application,
                       [] { QCoreApplication::quit(); });
    return application.exec();
}
