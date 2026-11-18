// httplib pulls in winsock2, which has to come before anything that includes windows.h.
#include <httplib.h>

#include "ui/DocsUi.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/Icons.h"
#include "ui/MenuSearch.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFrame>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <functional>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace lila::ui::docs
{
namespace
{
class DocsPanel;

/** Serves the docs copy on 127.0.0.1 for a browser when the public site can't be reached; docsify can't run from file://. */
class LocalServer
{
public:
    ~LocalServer() { stop(); }

    /** The server's base URL for root, (re)started on demand; empty when it can't listen. */
    std::string url(const fs::path& root)
    {
        if (mServer && mRoot != root)
            stop();
        if (!mServer)
        {
            auto server = std::make_unique<httplib::Server>();
            if (!server->set_mount_point("/", paths::toUtf8(root)))
                return {};
            server->set_post_routing_handler([](const httplib::Request&, httplib::Response& res) { res.set_header("Cache-Control", "no-store"); });
            int port = server->bind_to_any_port("127.0.0.1");
            if (port <= 0)
                return {};
            mPort = port;
            mRoot = root;
            mServer = std::move(server);
            mThread = std::thread([s = mServer.get()] { s->listen_after_bind(); });
        }
        return "http://127.0.0.1:" + std::to_string(mPort);
    }

    void stop()
    {
        if (mServer)
            mServer->stop();
        if (mThread.joinable())
            mThread.join();
        mServer.reset();
        mPort = 0;
    }

private:
    std::unique_ptr<httplib::Server> mServer;
    std::thread mThread;
    fs::path mRoot;
    int mPort = 0;
};

struct State
{
    QPointer<QThread> worker;
    std::atomic<bool> cancel{false};
    LocalServer server;
    QPointer<DocsPanel> panel;
    QPointer<QNetworkAccessManager> nam;
    std::string pendingPage;
    Route scopedRoute = Route::Auto;
    bool registered = false;
};

State& state()
{
    static State s;
    return s;
}

std::string shortSha(const std::string& sha)
{
    return sha.substr(0, 7);
}

/** One GET on the calling (worker) thread, aborted when the update is cancelled. */
Response qtFetch(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, const fs::path& saveTo)
{
    LILA_ZONE();

    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QString::fromStdString(url)));
    req.setTransferTimeout(10000);
    for (const auto& [k, v] : headers)
        req.setRawHeader(QByteArray::fromStdString(k), QByteArray::fromStdString(v));
    std::unique_ptr<QFile> file;
    if (!saveTo.empty())
    {
        file = std::make_unique<QFile>(QString::fromStdWString(saveTo.wstring()));
        if (!file->open(QIODevice::WriteOnly))
            return {.error = "cannot write " + paths::toUtf8(saveTo)};
    }
    std::unique_ptr<QNetworkReply> reply(nam.get(req));
    QByteArray body;
    bool writeFailed = false;
    auto drain = [&] {
        QByteArray chunk = reply->readAll();
        if (file)
            writeFailed = writeFailed || file->write(chunk) != chunk.size();
        else
            body += chunk;
    };
    QObject::connect(reply.get(), &QNetworkReply::readyRead, drain);
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (state().cancel)
            reply->abort();
    });
    poll.start(100);
    if (!reply->isFinished())
        loop.exec();
    drain();

    Response r;
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && r.status == 0)
        r.error = reply->errorString().toStdString();
    if (writeFailed)
        r = {.error = "cannot write " + paths::toUtf8(saveTo)};
    r.body = body.toStdString();
    return r;
}

void reloadPanel();

void syncFinished(const SyncResult& r, bool forced)
{
    state().worker = nullptr;
    std::optional<Root> root = currentRoot();
    std::string note = root && root->source == "override" ? " (the panel reads LILA_DOCS_DIR: " + paths::toUtf8(root->dir) + ")" : "";
    if (r.changed)
    {
        log::info("Docs updated to {} ({}){}", r.ref, shortSha(r.sha), note);
        reloadPanel();
    }
    else if (r.ok && forced)
        log::info("Docs are up to date ({} {}){}", r.ref, shortSha(r.sha), note);
    else if (!r.ok && forced)
        log::warn("Docs update failed: {}{}", r.message, root ? "; keeping the copy there is" : "");
    else if (!r.ok)
        log::debug("docs: no update: {}", r.message);
}

std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

/** The site's content column, 760 px less its padding at its 15 px font, in ems so it scales with the page's font. */
constexpr qreal kColumnEms = 48.7;
/** The site's body text against Lila's UI font, 15 px to 12 px at the default sizes. */
constexpr qreal kPageFontScale = 1.25;
/** Zoom levels in percent, a browser's steps. */
constexpr int kZoomSteps[] = {50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300};
constexpr const char* kZoomPref = "/docs/zoom";

/**
 * A font size in points that lands on a whole number of device pixels, so text is never drawn at a fractional size:
 * sitePx is the size on the site, at its 15 px body, scaled to a body of bodyPx.
 */
qreal crispPoints(qreal sitePx, qreal bodyPx, qreal dpr)
{
    const qreal device = std::max(1.0, std::round(sitePx * bodyPx / 15.0 * dpr));
    return device / dpr * 0.75;
}

/** Replaces each "<number><unit>" in css with what `to` makes of the number. */
QString scaleUnits(const QString& css, const QString& unit, const std::function<QString(qreal)>& to)
{
    const QRegularExpression re(R"((\d+(?:\.\d+)?))" + unit + R"(\b)");
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(css); it.hasNext();)
    {
        QRegularExpressionMatch m = it.next();
        out += css.mid(last, m.capturedStart() - last) + to(m.captured(1).toDouble());
        last = m.capturedEnd();
    }
    return out + css.mid(last);
}

/** theme.css's tokens. */
QString siteColors(QString css)
{
    static const std::pair<const char*, const char*> kColors[] = {{"@muted", "#a0a4ab"}, {"@text", "#e9eaec"},       {"@line", "#2f3338"},
                                                                  {"@panel", "#17191c"}, {"@accentSoft", "#8fbaff"}, {"@accent", "#3d8bff"},
                                                                  {"@ground", "#0c0d0f"}};
    for (const auto& [token, color] : kColors)
        css.replace(token, color);
    return css;
}

/**
 * The site's look (docsify's dark theme under the site's theme.css) in the CSS Qt's rich text reads. Lengths are in
 * "spx" and font sizes in "spt", both the site's pixels at its 15 px body, scaled to the page's.
 */
QString pageStyleSheet(qreal bodyPx, qreal dpr, const QString& mono)
{
    QString css = R"(
p, li { color: @muted; line-height: 170%; }
p { margin-top: 0; margin-bottom: 18spx; }
p.img { margin-top: 4spx; margin-bottom: 22spx; line-height: 100%; }
p.q { margin-top: 0; margin-bottom: 0; font-weight: 600; }
p.qn { margin-top: 10spx; margin-bottom: 0; font-weight: 600; }
h1, h2, h3, h4, h5, h6 { font-family: 'Archivo SemiExpanded', 'Archivo'; font-weight: 700; color: @text; }
h1 { font-size: 32spt; margin-top: 0; margin-bottom: 16spx; }
h2 { font-size: 28spt; margin-top: 10spx; margin-bottom: 13spx; }
h3 { font-size: 24spt; margin-top: 36spx; margin-bottom: 10spx; }
h4 { font-size: 20spt; margin-top: 28spx; margin-bottom: 8spx; }
h5, h6 { font-size: 16spt; margin-top: 24spx; margin-bottom: 8spx; }
hr { margin-top: 27spx; margin-bottom: 0; background-color: @line; }
b, strong { color: @text; font-weight: 600; }
a { color: @accentSoft; font-weight: 600; text-decoration: none; }
code { font-family: '@mono'; font-size: 13spt; color: @text; background-color: @panel; white-space: pre-wrap; }
pre { font-family: '@mono'; font-size: 13spt; color: @text; margin-top: 0; margin-bottom: 0; line-height: 150%; }
table.pre { background-color: @panel; border-color: @line; border-style: solid; margin-top: 2spx; margin-bottom: 18spx; }
table.pre td { padding: 14spx 21spx; }
table.quote { margin-top: 12spx; margin-bottom: 12spx; }
td.bar { background-color: @accent; }
td.body { padding-left: 20spx; padding-top: @quoteTop; padding-bottom: 7spx; color: @muted; }
table.grid { border-color: @line; border-style: solid; border-collapse: collapse; margin-top: 10spx; margin-bottom: 18spx; }
table.grid th { color: @text; font-weight: 600; padding: 6spx 13spx; }
table.grid td { color: @muted; padding: 6spx 13spx; }
td.alt { background-color: @panel; }
ul, ol { margin-top: 0; margin-bottom: 18spx; }
li { margin-bottom: 2spx; }
)";
    css = siteColors(css);
    css.replace("@mono", mono);
    // Qt puts a line's extra line-height below the text, so the quote's top padding takes as much again to centre it.
    QFont body("Archivo");
    body.setPointSizeF(crispPoints(15, bodyPx, dpr));
    const qreal extra = QFontMetricsF(body).height() * 0.7;
    css.replace("@quoteTop", QString::number(std::round(7 * bodyPx / 15.0 + extra)) + "px");
    css = scaleUnits(css, "spt", [&](qreal v) { return QString::number(crispPoints(v, bodyPx, dpr), 'f', 3) + "pt"; });
    return scaleUnits(css, "spx", [&](qreal v) { return QString::number(std::round(v * bodyPx / 15.0)) + "px"; });
}

/** The circle a middle click leaves where autoscroll started, as a browser draws it. */
class AutoScrollMark : public QWidget
{
public:
    explicit AutoScrollMark(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        resize(30, 30);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#a0a4ab"), 1.5));
        p.setBrush(QColor(23, 25, 28, 220));
        p.drawEllipse(QRectF(rect()).adjusted(1, 1, -1, -1));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e9eaec"));
        const QPointF c = QRectF(rect()).center();
        p.drawEllipse(c, 2, 2);
        p.drawPolygon(QPolygonF({c + QPointF(0, -10), c + QPointF(-4, -5), c + QPointF(4, -5)}));
        p.drawPolygon(QPolygonF({c + QPointF(0, 10), c + QPointF(-4, 5), c + QPointF(4, 5)}));
    }
};

/**
 * The page view: browser-style zoom keys and Ctrl+wheel in whole steps (the panel re-renders at the new size instead of
 * QTextEdit scaling the text), middle-click autoscroll, and copies without the narrow spaces that pad inline code.
 */
class PageBrowser : public QTextBrowser
{
public:
    explicit PageBrowser(QWidget* parent) : QTextBrowser(parent), mMark(new AutoScrollMark(viewport()))
    {
        mScrollTimer.setInterval(16);
        connect(&mScrollTimer, &QTimer::timeout, this, [this] { autoScrollStep(); });
    }

    /** Called with +1 or -1 to step the zoom, 0 to reset it. */
    std::function<void(int)> onZoom;
    std::function<void()> onDevicePixelRatio;

    bool autoScrolling() const { return mScrollTimer.isActive(); }

protected:
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ShortcutOverride)
        {
            auto* k = static_cast<QKeyEvent*>(e);
            if (zoomKey(k) != kNoZoom || (autoScrolling() && k->key() == Qt::Key_Escape))
            {
                e->accept();
                return true;
            }
        }
        if (e->type() == QEvent::DevicePixelRatioChange && onDevicePixelRatio)
            onDevicePixelRatio();
        return QTextBrowser::event(e);
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (int step = zoomKey(e); step != kNoZoom)
        {
            if (onZoom)
                onZoom(step);
            return;
        }
        if (e->key() == Qt::Key_Escape && autoScrolling())
        {
            stopAutoScroll();
            return;
        }
        QTextBrowser::keyPressEvent(e);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        if (!(e->modifiers() & Qt::ControlModifier))
        {
            QTextBrowser::wheelEvent(e);
            return;
        }
        mWheel += e->angleDelta().y();
        for (; std::abs(mWheel) >= 120; mWheel -= mWheel > 0 ? 120 : -120)
            if (onZoom)
                onZoom(mWheel > 0 ? 1 : -1);
        e->accept();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (autoScrolling())
        {
            stopAutoScroll();
            return;
        }
        if (e->button() == Qt::MiddleButton)
        {
            if (const QString href = anchorAt(e->position().toPoint()); !href.isEmpty())
                Q_EMIT anchorClicked(QUrl(href));
            else
                startAutoScroll(e->position().toPoint());
            return;
        }
        QTextBrowser::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        // Held and dragged, releasing ends it; after a plain click it runs until the next click.
        if (e->button() == Qt::MiddleButton && autoScrolling())
        {
            if (mDragged)
                stopAutoScroll();
            return;
        }
        QTextBrowser::mouseReleaseEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!autoScrolling())
            QTextBrowser::mouseMoveEvent(e);
    }

    void focusOutEvent(QFocusEvent* e) override
    {
        stopAutoScroll();
        QTextBrowser::focusOutEvent(e);
    }

    QMimeData* createMimeDataFromSelection() const override
    {
        QMimeData* mime = QTextBrowser::createMimeDataFromSelection();
        if (mime && mime->hasText())
            mime->setText(mime->text().remove(QChar(0x202F)).replace(QChar(0x2011), QChar('-')));
        return mime;
    }

private:
    static constexpr int kNoZoom = 99;
    static constexpr qreal kDeadZone = 10;

    static int zoomKey(const QKeyEvent* e)
    {
        if (!(e->modifiers() & Qt::ControlModifier) || (e->modifiers() & (Qt::AltModifier | Qt::MetaModifier)))
            return kNoZoom;
        switch (e->key())
        {
        case Qt::Key_0: return 0;
        case Qt::Key_Equal:
        case Qt::Key_Plus: return 1;
        case Qt::Key_Minus:
        case Qt::Key_Underscore: return -1;
        default: return kNoZoom;
        }
    }

    void startAutoScroll(const QPoint& at)
    {
        mOrigin = at;
        mDragged = false;
        mCarry = 0;
        mMark->move(at - QPoint(mMark->width() / 2, mMark->height() / 2));
        mMark->show();
        mMark->raise();
        viewport()->setCursor(Qt::SizeVerCursor);
        setFocus(Qt::MouseFocusReason);
        mScrollTimer.start();
    }

    void stopAutoScroll()
    {
        if (!autoScrolling())
            return;
        mScrollTimer.stop();
        mMark->hide();
        viewport()->unsetCursor();
    }

    /** Scrolls at a speed that grows with the cursor's distance from where it started, past a small dead zone. */
    void autoScrollStep()
    {
        const qreal dy = viewport()->mapFromGlobal(QCursor::pos()).y() - mOrigin.y();
        if (std::abs(dy) <= kDeadZone)
            return;
        mDragged = mDragged || (QGuiApplication::mouseButtons() & Qt::MiddleButton);
        mCarry += (dy - std::copysign(kDeadZone, dy)) * 0.15;
        const int whole = int(mCarry);
        mCarry -= whole;
        verticalScrollBar()->setValue(verticalScrollBar()->value() + whole);
    }

    AutoScrollMark* mMark;
    QTimer mScrollTimer;
    QPoint mOrigin;
    bool mDragged = false;
    qreal mCarry = 0;
    int mWheel = 0;
};

constexpr const char* kBookmarksPref = "/docs/bookmarks";

std::vector<Bookmark> loadBookmarks()
{
    return bookmarksFromJson(PrefStore::get().value("general", kBookmarksPref, json::array()));
}

void saveBookmarks(const std::vector<Bookmark>& list)
{
    PrefStore::get().set("general", kBookmarksPref, bookmarksToJson(list));
}

std::string specFor(const std::string& page, const std::string& anchor)
{
    return page + (anchor.empty() ? "" : "?id=" + anchor);
}

void copyToClipboard(const std::string& url)
{
    QGuiApplication::clipboard()->setText(qs(url));
    log::info("Copied {}", url);
}

Route resolveRoute(Route route)
{
    if (route == Route::Auto)
        route = state().scopedRoute;
    if (route == Route::Auto)
        route = (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) ? Route::Browser : Route::Panel;
    return route;
}

class DocsPanel : public QWidget
{
    /** On a sidebar entry for a section of the current page: its anchor. */
    static constexpr int kHeadingRole = Qt::UserRole + 2;

public:
    explicit DocsPanel(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto* bar = new QToolBar(this);
        bar->setIconSize(QSize(16, 16));
        mBackAct = bar->addAction(icon("arrow-left"), "Back", [this] { goHistory(-1); });
        mForwardAct = bar->addAction(icon("arrow-right"), "Forward", [this] { goHistory(1); });
        bar->addAction(icon("home"), "Home", [this] { navigate({}); })->setToolTip("Docs home");
        mSearch = new QLineEdit(bar);
        mSearch->setPlaceholderText("Search");
        mSearch->setClearButtonEnabled(true);
        mSearch->addAction(icon("search"), QLineEdit::LeadingPosition);
        bar->addWidget(mSearch);
        mSearch->setToolTip("Search the docs, or paste a docs link and press Enter to open it");
        mStarAct = bar->addAction(icon("star"), "Bookmark", [this] { toggleStar(); });
        mStarAct->setCheckable(true);
        mStarAct->setToolTip("Bookmark this page at the heading shown at the top");
        bar->addAction(icon("link"), "Copy Link to This Page", [this] { copyToClipboard(currentUrl()); })
            ->setToolTip("Copy the docs website's link to this page and the heading shown at the top");
        bar->addAction(icon("refresh"), "Update Docs", [] { requestUpdate(true); })->setToolTip("Download the latest docs");
        bar->addAction(icon("external-link"), "Open in Browser", [this] { openInBrowser(specFor(mCurrent.id, nearestAnchor())); })
            ->setToolTip("Open this page on the docs website, or from the local copy when offline");
        layout->addWidget(bar);

        auto* split = new QSplitter(Qt::Horizontal, this);
        split->setChildrenCollapsible(false);
        auto* leftSplit = new QSplitter(Qt::Vertical, split);
        leftSplit->setChildrenCollapsible(false);
        mBookmarks = new QListWidget(leftSplit);
        mBookmarks->setDragDropMode(QAbstractItemView::InternalMove);
        mBookmarks->setEditTriggers(QAbstractItemView::EditKeyPressed);
        mBookmarks->setContextMenuPolicy(Qt::CustomContextMenu);
        mBookmarks->setToolTip("Bookmarks: drag to reorder, F2 to rename, Delete to remove");
        deletion::claim(mBookmarks, [this] { removeBookmark(mBookmarks->currentRow()); });
        mLeft = new QStackedWidget(leftSplit);
        mSidebar = new QTreeWidget(mLeft);
        mSidebar->setHeaderHidden(true);
        mSidebar->setRootIsDecorated(false);
        mSidebar->setIndentation(14);
        mSidebar->setSelectionMode(QAbstractItemView::NoSelection);
        mSidebar->setStyleSheet(siteColors("QTreeWidget { background-color: @panel; color: @muted; border: none; outline: 0; padding: 10px 6px 10px 12px; }"
                                           "QTreeWidget::item { padding: 3px 0px; border: none; }"
                                           "QTreeWidget::item:hover, QTreeWidget::item:selected { background: transparent; }"));
        mResults = new QListWidget(mLeft);
        mResults->setWordWrap(true);
        mResults->setTextElideMode(Qt::ElideNone);
        mLeft->addWidget(mSidebar);
        mLeft->addWidget(mResults);
        leftSplit->addWidget(mBookmarks);
        leftSplit->addWidget(mLeft);
        leftSplit->setStretchFactor(1, 1);
        leftSplit->setSizes({120, 400});
        auto* page = new PageBrowser(split);
        page->onZoom = [this](int step) { zoom(step); };
        page->onDevicePixelRatio = [this] { rerender(); };
        mBrowser = page;
        mBrowser->setOpenLinks(false);
        mBrowser->setStyleSheet(siteColors("QTextBrowser { background-color: @ground; color: @muted; border: none; }"));
        mZoom = std::clamp(PrefStore::get().value<int>("general", kZoomPref, 100), kZoomSteps[0], std::end(kZoomSteps)[-1]);
        stylePage();
        mBrowser->setContextMenuPolicy(Qt::CustomContextMenu);
        split->addWidget(leftSplit);
        split->addWidget(mBrowser);
        split->setStretchFactor(1, 1);
        split->setSizes({220, 780});
        layout->addWidget(split, 1);

        connect(mSidebar, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
            QString target = item->data(0, Qt::UserRole).toString();
            const std::string anchor = item->data(0, kHeadingRole).toString().toStdString();
            if (target.isEmpty() && anchor.empty())
                return;
            if (item->data(0, Qt::UserRole + 1).toBool())
                QDesktopServices::openUrl(QUrl(target));
            else if (!anchor.empty() && mHasCurrent && item->parent() == mSidebarPage)
                scrollToAnchor(anchor);
            else
                navigate(target.toStdString());
        });
        connect(mResults, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            navigate(item->data(Qt::UserRole).toString().toStdString());
            highlightQuery();
        });
        connect(mBrowser, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) { followLink(url); });
        connect(mBrowser, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showBrowserMenu(pos); });
        connect(mBrowser->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
            updateStar();
            highlightSidebar();
        });
        connect(mBookmarks, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            if (const Bookmark* b = bookmarkFor(item))
                navigate(specFor(b->page, b->anchor));
        });
        connect(mBookmarks, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) { renameBookmark(item); });
        connect(mBookmarks, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showBookmarkMenu(pos); });
        connect(mBookmarks->model(), &QAbstractItemModel::rowsMoved, this, [this] {
            // After the drop finishes: the list is still mid-move while this runs.
            QTimer::singleShot(0, this, [this] { saveBookmarkOrder(); });
        });

        mSearchTimer.setSingleShot(true);
        mSearchTimer.setInterval(150);
        connect(&mSearchTimer, &QTimer::timeout, this, [this] { runSearch(); });
        connect(mSearch, &QLineEdit::textChanged, this, [this] { mSearchTimer.start(); });
        connect(mSearch, &QLineEdit::returnPressed, this, [this] {
            if (std::optional<std::string> spec = pageSpecFromUrl(mSearch->text().toStdString()))
            {
                mSearch->clear();
                navigate(*spec);
                return;
            }
            runSearch();
            if (mResults->count() > 0)
                Q_EMIT mResults->itemClicked(mResults->item(0));
        });
        mFitTimer.setSingleShot(true);
        mFitTimer.setInterval(100);
        connect(&mFitTimer, &QTimer::timeout, this, [this] { fitImages(); });
        // Only once every widget exists: the toolbar sends its children events while it is still being built.
        mSearch->installEventFilter(this);
        mBookmarks->installEventFilter(this);
        mBrowser->viewport()->installEventFilter(this);

        state().panel = this;
        fillBookmarks();
        reload();
        navigate(std::exchange(state().pendingPage, {}), false);
    }

    /** Re-reads the sidebar and the current page, as after an update. */
    void reload()
    {
        mRoot = currentRoot();
        fillSidebar();
        fillBookmarks();
        mLoadedFile.clear();
        if (mHasCurrent)
            navigate(mCurrent.id + (mCurrent.anchor.empty() ? "" : "?id=" + mCurrent.anchor), false);
    }

    void navigate(const std::string& spec, bool push = true)
    {
        LILA_ZONE();

        mRoot = currentRoot();
        if (!mRoot)
        {
            mBrowser->setHtml("<p>No docs here yet. <b>Help &gt; Update Docs</b> downloads them; it needs an internet connection "
                              "once.</p>");
            mHasCurrent = false;
            updateButtons();
            return;
        }
        Page page = resolvePage(docsDir(), spec);
        Loc next{page.id, page.anchor};
        if (push && mHasCurrent && (next.id != mCurrent.id || next.anchor != mCurrent.anchor))
        {
            mBack.push_back(mCurrent);
            mForward.clear();
        }
        showPage(page);
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (!mBrowser)
            return QWidget::eventFilter(o, e);
        if (o == mSearch && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape)
        {
            mSearch->clear();
            return true;
        }
        if (o == mBookmarks && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Delete)
        {
            removeBookmark(mBookmarks->currentRow());
            return true;
        }
        if (o == mBrowser->viewport() && e->type() == QEvent::Resize)
        {
            centerColumn();
            mFitTimer.start();
        }
        if (o == mBrowser->viewport() && e->type() == QEvent::MouseButtonPress)
        {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::BackButton || me->button() == Qt::ForwardButton)
            {
                goHistory(me->button() == Qt::BackButton ? -1 : 1);
                return true;
            }
        }
        return QWidget::eventFilter(o, e);
    }

private:
    struct Loc
    {
        std::string id;
        std::string anchor;
    };

    fs::path docsDir() const { return mRoot ? mRoot->dir / "docs" : fs::path(); }

    void showPage(const Page& page)
    {
        if (page.file != mLoadedFile)
        {
            fs::path dir = page.file.parent_path();
            mBrowser->setSearchPaths({QString::fromStdWString(dir.wstring()), QString::fromStdWString(docsDir().wstring())});
            stylePage();
            HtmlOptions options;
            options.imageSize = [this](std::string_view src) -> std::optional<std::pair<int, int>> {
                QSize size = imageSize(qs(std::string(src)));
                return size.isEmpty() ? std::nullopt : std::optional(std::pair{size.width(), size.height()});
            };
            options.maxImageWidth = int(maxImageWidth());
            mBrowser->setHtml(qs(markdownToHtml(readText(page.file), options)));
            mLoadedFile = page.file;
            centerColumn();
        }
        mCurrent = {page.id, page.anchor};
        mHasCurrent = true;
        if (page.anchor.empty() || !scrollToAnchor(page.anchor))
            mBrowser->verticalScrollBar()->setValue(0);
        selectSidebar(page.id);
        updateButtons();
        updateStar();
    }

    void goHistory(int step)
    {
        auto& from = step < 0 ? mBack : mForward;
        auto& to = step < 0 ? mForward : mBack;
        if (from.empty() || !mRoot)
            return;
        if (mHasCurrent)
            to.push_back(mCurrent);
        Loc loc = from.back();
        from.pop_back();
        showPage(resolvePage(docsDir(), loc.id + (loc.anchor.empty() ? "" : "?id=" + loc.anchor)));
    }

    void updateButtons()
    {
        mBackAct->setEnabled(!mBack.empty());
        mForwardAct->setEnabled(!mForward.empty());
    }

    void followLink(const QUrl& url)
    {
        const QString scheme = url.scheme();
        if (scheme == "http" || scheme == "https" || scheme == "mailto")
        {
            QDesktopServices::openUrl(url);
            return;
        }
        QString s = url.toString();
        if (s.startsWith('#') && !s.startsWith("#/"))
        {
            scrollToAnchor(s.mid(1).toStdString());
            return;
        }
        navigate(s.toStdString());
    }

    bool scrollToAnchor(const std::string& anchor)
    {
        for (QTextBlock b = mBrowser->document()->begin(); b.isValid(); b = b.next())
            if (b.blockFormat().headingLevel() > 0 && slug(b.text().toStdString()) == anchor)
            {
                qreal y = mBrowser->document()->documentLayout()->blockBoundingRect(b).top();
                mBrowser->verticalScrollBar()->setValue(int(y));
                return true;
            }
        return false;
    }

    qreal em() const { return QFontInfo(mBrowser->document()->defaultFont()).pixelSize(); }

    /** The page's fonts and stylesheet, sized from the panel's font so the UI font preference scales the page too. */
    /** The site's body size for Lila's UI font, before zoom. */
    qreal basePx() const { return std::max(10, QFontInfo(font()).pixelSize()) * kPageFontScale; }

    /**
     * The page's fonts and stylesheet at the current zoom. Sizes snap to whole device pixels and hinting matches the UI
     * font, so the text is as crisp as the rest of Lila at any zoom or display scale.
     */
    void stylePage()
    {
        QTextDocument* doc = mBrowser->document();
        const qreal px = basePx() * mZoom / 100.0;
        const qreal dpr = mBrowser->devicePixelRatioF();
        QFont body("Archivo");
        body.setPointSizeF(crispPoints(15, px, dpr));
        body.setHintingPreference(QFont::PreferNoHinting);
        doc->setDefaultFont(body);
        doc->setDocumentMargin(std::round(px * 1.6));
        doc->setIndentWidth(std::round(px * 1.6));
        doc->setDefaultStyleSheet(pageStyleSheet(px, dpr, monoFont().family()));

        QFont side("Archivo");
        side.setPointSizeF(crispPoints(14, basePx(), dpr));
        side.setHintingPreference(QFont::PreferNoHinting);
        mSidebar->setFont(side);
    }

    /** Steps the zoom through kZoomSteps, or back to 100% for 0, and re-renders the page at the new size. */
    void zoom(int step)
    {
        const int* at = std::lower_bound(std::begin(kZoomSteps), std::end(kZoomSteps), mZoom);
        const int index = int(at - std::begin(kZoomSteps));
        const int count = int(std::size(kZoomSteps));
        const int next = step == 0 ? 100 : kZoomSteps[std::clamp(step > 0 && *at == mZoom ? index + 1 : step > 0 ? index : index - 1, 0, count - 1)];
        if (next == mZoom)
            return;
        mZoom = next;
        PrefStore::get().set("general", kZoomPref, mZoom);
        rerender();
    }

    /** Lays the page out again after a zoom or display scale change, keeping the reader's place in it. */
    void rerender()
    {
        QScrollBar* bar = mBrowser->verticalScrollBar();
        const double place = bar->maximum() > 0 ? double(bar->value()) / bar->maximum() : 0.0;
        stylePage();
        if (!mHasCurrent || !mRoot)
            return;
        mLoadedFile.clear();
        showPage(resolvePage(docsDir(), mCurrent.id));
        mBrowser->document()->size();
        bar->setValue(int(std::lround(place * bar->maximum())));
    }

    /** An image's own size, looked up as Qt resolves its src: against the page's folder, then the docs root. */
    QSize imageSize(const QString& src) const
    {
        for (const QString& dir : mBrowser->searchPaths())
            if (QImageReader reader(dir + "/" + src); reader.canRead())
                return reader.size();
        return {};
    }

    qreal maxImageWidth() const
    {
        const qreal room = mBrowser->viewport()->width() - 2 * mBrowser->document()->documentMargin();
        return std::max(64.0, std::min(kColumnEms * em(), room));
    }

    /** The site's column: at most kColumnEms wide and centred, the side margins growing with the panel. */
    void centerColumn()
    {
        QTextDocument* doc = mBrowser->document();
        // The root frame's side margins replace the document margin, so they never go below it.
        const qreal side = std::max<qreal>(doc->documentMargin(), (mBrowser->viewport()->width() - kColumnEms * em()) / 2);
        QTextFrameFormat f = doc->rootFrame()->frameFormat();
        if (qFuzzyCompare(f.leftMargin(), side) && qFuzzyCompare(f.rightMargin(), side))
            return;
        f.setLeftMargin(side);
        f.setRightMargin(side);
        doc->rootFrame()->setFrameFormat(f);
        doc->setModified(false);
    }

    /** Screenshots shrink to the panel's width and grow back to their own size, never past it. */
    void fitImages()
    {
        QTextDocument* doc = mBrowser->document();
        const qreal maxW = maxImageWidth();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it)
            {
                QTextFragment frag = it.fragment();
                if (!frag.isValid() || !frag.charFormat().isImageFormat())
                    continue;
                QTextImageFormat img = frag.charFormat().toImageFormat();
                const QSize natural = imageSize(img.name());
                if (natural.isEmpty())
                    continue;
                qreal w = std::min<qreal>(natural.width(), maxW);
                if (qFuzzyCompare(img.width(), w))
                    continue;
                img.setWidth(w);
                img.setHeight(natural.height() * w / natural.width());
                QTextCursor fc(doc);
                fc.setPosition(frag.position());
                fc.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
                fc.setCharFormat(img);
            }
        doc->setModified(false);
    }

    void fillSidebar()
    {
        mSidebar->clear();
        if (!mRoot)
            return;
        std::vector<QTreeWidgetItem*> parents;
        for (const SidebarEntry& e : readSidebar(docsDir()))
        {
            parents.resize(std::min<size_t>(parents.size(), size_t(e.depth)));
            auto* item = parents.empty() ? new QTreeWidgetItem(mSidebar) : new QTreeWidgetItem(parents.back());
            item->setText(0, qs(e.title));
            item->setData(0, Qt::UserRole, qs(e.target));
            item->setData(0, Qt::UserRole + 1, e.external);
            if (e.label)
            {
                item->setFlags(Qt::ItemIsEnabled);
                QFont f = item->font(0);
                f.setCapitalization(QFont::AllUppercase);
                f.setPointSizeF(f.pointSizeF() * 0.85);
                item->setFont(0, f);
                item->setForeground(0, mSidebar->palette().color(QPalette::PlaceholderText));
            }
            parents.push_back(item);
        }
        mSidebarPage = nullptr;
        mSidebar->expandAll();
    }

    /** The current page's entry, expanded to list its h2 sections as the site's sidebar does; other pages stay closed. */
    void selectSidebar(const std::string& id)
    {
        if (mSidebarPage)
            qDeleteAll(mSidebarPage->takeChildren());
        mSidebarPage = nullptr;
        QTreeWidgetItemIterator it(mSidebar);
        for (; *it; ++it)
            if (!(*it)->data(0, Qt::UserRole + 1).toBool() && (*it)->data(0, kHeadingRole).toString().isEmpty() &&
                (*it)->data(0, Qt::UserRole).toString().toStdString() == id)
                mSidebarPage = *it;
        if (mSidebarPage && mSidebarPage->childCount() == 0)
        {
            for (QTextBlock b = mBrowser->document()->begin(); b.isValid(); b = b.next())
                if (b.blockFormat().headingLevel() == 2)
                {
                    auto* item = new QTreeWidgetItem(mSidebarPage);
                    const std::string anchor = slug(b.text().toStdString());
                    item->setText(0, b.text());
                    item->setData(0, Qt::UserRole, qs(specFor(id, anchor)));
                    item->setData(0, kHeadingRole, qs(anchor));
                }
            mSidebarPage->setExpanded(true);
        }
        if (mSidebarPage)
            mSidebar->scrollToItem(mSidebarPage);
        highlightSidebar();
    }

    /** The h2 at or above the top of the view, as an anchor. */
    std::string currentSection() const
    {
        if (!mHasCurrent || mBrowser->verticalScrollBar()->value() == 0)
            return {};
        for (QTextBlock b = mBrowser->cursorForPosition(QPoint(0, 4)).block(); b.isValid(); b = b.previous())
            if (int level = b.blockFormat().headingLevel(); level == 1 || level == 2)
                return level == 2 ? slug(b.text().toStdString()) : std::string();
        return {};
    }

    /** The current page and the section at the top of the view in the site's accent, the rest muted. */
    void highlightSidebar()
    {
        const std::string section = currentSection();
        QFont bold = mSidebar->font();
        bold.setWeight(QFont::DemiBold);
        for (QTreeWidgetItemIterator it(mSidebar); *it; ++it)
        {
            QTreeWidgetItem* item = *it;
            const QString anchor = item->data(0, kHeadingRole).toString();
            const bool active = anchor.isEmpty() ? item == mSidebarPage : item->parent() == mSidebarPage && anchor.toStdString() == section;
            item->setFont(0, active ? bold : mSidebar->font());
            item->setForeground(0, QColor(active ? "#8fbaff" : item->data(0, Qt::UserRole + 1).toBool() ? "#7d828a" : "#a0a4ab"));
        }
    }

    void runSearch()
    {
        LILA_ZONE();

        const QString q = mSearch->text().trimmed();
        mResults->clear();
        mLeft->setCurrentWidget(q.isEmpty() ? static_cast<QWidget*>(mSidebar) : mResults);
        if (q.isEmpty() || !mRoot)
        {
            mBrowser->setExtraSelections({});
            return;
        }
        std::vector<Hit> hits = search(docsDir(), q.toStdString());
        for (const Hit& h : hits)
        {
            auto* item = new QListWidgetItem(qs(h.title) + (h.snippet.empty() ? "" : "\n" + qs(h.snippet)), mResults);
            item->setData(Qt::UserRole, qs(h.pageId + (h.anchor.empty() ? "" : "?id=" + h.anchor)));
        }
        if (hits.empty())
            new QListWidgetItem("No results", mResults);
    }

    void highlightQuery()
    {
        QList<QTextEdit::ExtraSelection> marks;
        QTextCharFormat f;
        f.setBackground(themeShade(90));
        for (const QString& word : mSearch->text().split(' ', Qt::SkipEmptyParts))
        {
            // Inline code shows its hyphens as non-breaking ones.
            const QRegularExpression re(QRegularExpression::escape(word).replace("\\-", "[-\\x{2011}]"),
                                        QRegularExpression::CaseInsensitiveOption);
            for (QTextCursor c = mBrowser->document()->find(re); !c.isNull(); c = mBrowser->document()->find(re, c))
                marks.append({c, f});
        }
        mBrowser->setExtraSelections(marks);
    }

    /** The heading at the top of the view, as an anchor; empty at the top of the page or above its first heading. */
    std::string nearestAnchor(QString* text = nullptr) const
    {
        if (!mHasCurrent || mBrowser->verticalScrollBar()->value() == 0)
            return {};
        for (QTextBlock b = mBrowser->cursorForPosition(QPoint(0, 4)).block(); b.isValid(); b = b.previous())
            if (int level = b.blockFormat().headingLevel(); level > 0)
            {
                if (level == 1)
                    return {};
                if (text)
                    *text = b.text();
                return slug(b.text().toStdString());
            }
        return {};
    }

    QString pageTitle() const
    {
        for (QTextBlock b = mBrowser->document()->begin(); b.isValid(); b = b.next())
            if (b.blockFormat().headingLevel() == 1)
                return b.text();
        return mCurrent.id.empty() ? QString("Home") : qs(mCurrent.id);
    }

    std::string currentUrl() const { return publicUrl(mCurrent.id, nearestAnchor()); }

    void showBrowserMenu(const QPoint& pos)
    {
        const QPoint docPos = pos + QPoint(mBrowser->horizontalScrollBar()->value(), mBrowser->verticalScrollBar()->value());
        std::unique_ptr<QMenu> menu(mBrowser->createStandardContextMenu(docPos));
        const QString href = mBrowser->anchorAt(pos);
        auto* copyLink = new QAction("Copy Link Location", menu.get());
        copyLink->setEnabled(!href.isEmpty());
        connect(copyLink, &QAction::triggered, this, [this, href] { copyToClipboard(publicUrlForLink(href.toStdString(), mCurrent.id)); });
        QAction* stock = nullptr;
        for (QAction* a : menu->actions())
            if (a->objectName() == "link-copy" || a->text().contains("Link Location"))
                stock = a;
        menu->insertAction(stock ? stock : menu->actions().value(0), copyLink);
        if (stock)
            menu->removeAction(stock);
        menu->addSeparator();
        menu->addAction(icon("link"), "Copy Link to This Page", this, [this] { copyToClipboard(currentUrl()); });
        menu->exec(mBrowser->viewport()->mapToGlobal(pos));
    }

    const Bookmark* bookmarkFor(QListWidgetItem* item) const
    {
        int i = item ? item->data(Qt::UserRole).toInt() : -1;
        return i >= 0 && i < int(mBookmarkList.size()) ? &mBookmarkList[size_t(i)] : nullptr;
    }

    void fillBookmarks()
    {
        mBookmarkList = loadBookmarks();
        QSignalBlocker block(mBookmarks);
        mBookmarks->clear();
        for (size_t i = 0; i < mBookmarkList.size(); ++i)
        {
            const Bookmark& b = mBookmarkList[i];
            auto* item = new QListWidgetItem(icon("bookmark"), qs(b.title), mBookmarks);
            item->setData(Qt::UserRole, int(i));
            item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
            const std::string spec = specFor(b.page, b.anchor);
            // Kept when a docs update drops the page, so it comes back if the page does.
            if (mRoot && !resolvePage(docsDir(), spec).found)
            {
                item->setForeground(themeShade(110));
                item->setToolTip("Not in these docs: " + qs(spec));
            }
            else
                item->setToolTip(qs(spec.empty() ? "Home" : spec));
        }
        mBookmarks->setVisible(!mBookmarkList.empty());
        updateStar();
    }

    void updateStar()
    {
        if (!mStarAct)
            return;
        mStarAct->setEnabled(mHasCurrent);
        mStarAct->setChecked(mHasCurrent && findBookmark(mBookmarkList, mCurrent.id, nearestAnchor()) >= 0);
    }

    void toggleStar()
    {
        if (!mHasCurrent)
            return;
        QString heading;
        std::string anchor = nearestAnchor(&heading);
        QString title = pageTitle() + (heading.isEmpty() ? QString() : " > " + heading);
        std::vector<Bookmark> list = loadBookmarks();
        toggleBookmark(list, {title.toStdString(), mCurrent.id, anchor});
        saveBookmarks(list);
        fillBookmarks();
    }

    void renameBookmark(QListWidgetItem* item)
    {
        int i = item->data(Qt::UserRole).toInt();
        std::string title = item->text().trimmed().toStdString();
        if (i < 0 || i >= int(mBookmarkList.size()))
            return;
        if (!title.empty())
        {
            mBookmarkList[size_t(i)].title = title;
            saveBookmarks(mBookmarkList);
        }
        fillBookmarks();
    }

    void removeBookmark(int row)
    {
        QListWidgetItem* item = mBookmarks->item(row);
        int i = item ? item->data(Qt::UserRole).toInt() : -1;
        if (i < 0 || i >= int(mBookmarkList.size()))
            return;
        mBookmarkList.erase(mBookmarkList.begin() + i);
        saveBookmarks(mBookmarkList);
        fillBookmarks();
    }

    void moveBookmarkRow(int row, int step)
    {
        if (row < 0 || row + step < 0 || row + step >= mBookmarks->count())
            return;
        saveBookmarkOrder();
        moveBookmark(mBookmarkList, row, row + step);
        saveBookmarks(mBookmarkList);
        fillBookmarks();
        mBookmarks->setCurrentRow(row + step);
    }

    /** The list's order, after a drag, back into the prefs. */
    void saveBookmarkOrder()
    {
        std::vector<Bookmark> ordered;
        for (int r = 0; r < mBookmarks->count(); ++r)
            if (const Bookmark* b = bookmarkFor(mBookmarks->item(r)))
                ordered.push_back(*b);
        if (ordered.size() != mBookmarkList.size() || ordered == mBookmarkList)
            return;
        saveBookmarks(ordered);
        fillBookmarks();
    }

    void showBookmarkMenu(const QPoint& pos)
    {
        QListWidgetItem* item = mBookmarks->itemAt(pos);
        if (!item)
            return;
        const int row = mBookmarks->row(item);
        QMenu menu;
        menu.addAction("Open", this, [this, item] { Q_EMIT mBookmarks->itemClicked(item); });
        menu.addAction("Rename", this, [this, item] { mBookmarks->editItem(item); });
        menu.addAction("Move Up", this, [this, row] { moveBookmarkRow(row, -1); })->setEnabled(row > 0);
        menu.addAction("Move Down", this, [this, row] { moveBookmarkRow(row, 1); })->setEnabled(row + 1 < mBookmarks->count());
        menu.addSeparator();
        menu.addAction("Remove", this, [this, row] { removeBookmark(row); });
        menu.exec(mBookmarks->viewport()->mapToGlobal(pos));
    }

    QAction* mBackAct = nullptr;
    QAction* mForwardAct = nullptr;
    QAction* mStarAct = nullptr;
    QListWidget* mBookmarks = nullptr;
    QLineEdit* mSearch = nullptr;
    QStackedWidget* mLeft = nullptr;
    QTreeWidget* mSidebar = nullptr;
    QTreeWidgetItem* mSidebarPage = nullptr;
    int mZoom = 100;
    QListWidget* mResults = nullptr;
    QTextBrowser* mBrowser = nullptr;
    QTimer mSearchTimer;
    QTimer mFitTimer;

    std::optional<Root> mRoot;
    fs::path mLoadedFile;
    Loc mCurrent;
    bool mHasCurrent = false;
    std::vector<Loc> mBack;
    std::vector<Loc> mForward;
    std::vector<Bookmark> mBookmarkList;
};

void reloadPanel()
{
    if (DocsPanel* p = state().panel)
        p->reload();
}

std::string pageArg(const Args& a)
{
    std::string page = a.get<std::string>("page", "");
    if (page.empty() && !a.positional.empty() && a.positional[0].is_string())
        page = a.positional[0].get<std::string>();
    return page;
}
} // namespace

std::optional<Root> currentRoot()
{
    return selectRoot(defaultSources());
}

void requestUpdate(bool forced)
{
    State& s = state();
    if (s.worker)
    {
        if (forced)
            log::info("Docs: an update is already running");
        return;
    }
    Sources src = defaultSources();
    s.cancel = false;
    QThread* t = QThread::create([src, forced] {
        SyncResult r = sync(qtFetch, src.cacheRoot, src.refs, &state().cancel);
        QMetaObject::invokeMethod(qApp, [r, forced] { syncFinished(r, forced); }, Qt::QueuedConnection);
    });
    t->setObjectName("DocsUpdate");
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    s.worker = t;
    if (forced)
        log::info("Docs: checking for updates...");
    t->start(QThread::LowPriority);
}

void startAutoUpdate()
{
    if (!PrefStore::get().value<bool>("general", "/docs/autoUpdate", true))
        return;
    // An authoring checkout is read as is; there is nothing to fetch for it.
    if (isSiteRoot(defaultSources().overrideDir))
        return;
    requestUpdate(false);
}

void shutdown()
{
    State& s = state();
    s.cancel = true;
    if (QThread* t = s.worker)
        t->wait(5000);
    s.server.stop();
}

RouteScope::RouteScope(Route route) : mPrevious(state().scopedRoute)
{
    state().scopedRoute = route;
}

RouteScope::~RouteScope()
{
    state().scopedRoute = mPrevious;
}

void open(const std::string& page, Route route)
{
    if (resolveRoute(route) == Route::Browser)
    {
        openInBrowser(page);
        return;
    }
    panels::show(kDocsDock);
    if (DocsPanel* p = state().panel)
        p->navigate(page);
    else
        state().pendingPage = page;
}

bool openApi(const std::string& symbol, Route route)
{
    std::optional<Root> root = currentRoot();
    if (!root)
        return false;
    std::string page = apiPageFor(root->dir / "docs", symbol);
    if (page.empty())
        return false;
    open(page, route);
    return true;
}

void openInBrowser(const std::string& page)
{
    std::string anchor;
    std::string id = normalizePageId(page, &anchor);
    std::string publicPage = publicUrl(id, anchor);
    State& s = state();
    if (!s.nam)
        s.nam = new QNetworkAccessManager(qApp);
    QNetworkRequest req{QUrl(kPublicSite)};
    req.setTransferTimeout(3000);
    QNetworkReply* reply = s.nam->head(req);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, publicPage, id, anchor] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
        {
            QDesktopServices::openUrl(QUrl(qs(publicPage)));
            return;
        }
        std::optional<Root> root = currentRoot();
        std::string base = root ? state().server.url(root->dir) : std::string();
        if (base.empty())
        {
            log::warn("Docs: the website can't be reached and there is no local copy to serve");
            return;
        }
        log::info("Docs: the website can't be reached, opening the local copy");
        QDesktopServices::openUrl(QUrl(qs(base + "/docs/#/" + specFor(id, anchor))));
    });
}

std::string pageTitle(const std::string& page)
{
    std::optional<Root> root = currentRoot();
    return docs::pageTitle(root ? root->dir / "docs" : fs::path(), page);
}

namespace
{
/** A sidebar title in the title case of menu labels: "Reference images" reads "Reference Images"; small words and names with dots stay. */
std::string labelCase(const std::string& title)
{
    static const std::set<std::string> kSmall = {"a", "an", "and", "as", "at", "by", "for", "in", "of", "on", "or", "the", "to", "vs"};
    std::string out;
    size_t i = 0;
    while (i < title.size())
    {
        const size_t end = std::min(title.find(' ', i), title.size());
        std::string word = title.substr(i, end - i);
        const bool first = out.empty();
        if (!word.empty() && word.find('.') == std::string::npos && (first || !kSmall.contains(word)))
            word[0] = char(std::toupper(static_cast<unsigned char>(word[0])));
        out += word;
        if (end < title.size())
            out += ' ';
        i = end + 1;
    }
    return out;
}
} // namespace

std::string pageAction(const std::string& page)
{
    std::string id = pageActionId(page);
    if (!actions::find(id))
    {
        std::string title = labelCase(pageTitle(page));
        actions::add({.id = id,
                      .label = "Open " + title + " Docs",
                      .category = "Help",
                      .run = [page] { open(page); },
                      .doc = "Opens the " + title + " page of the docs. Shift+click opens it in the web browser.",
                      .python = "lila.docs.open(\"" + page + "\")"});
    }
    return id;
}

QToolButton* helpButton(const std::string& page, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon("book"));
    b->setIconSize(QSize(14, 14));
    b->setToolTip(qs("Open " + pageTitle(page) + " docs"));
    b->setAutoRaise(true);
    b->setFixedSize(18, 18);
    b->setFocusPolicy(Qt::NoFocus);
    QObject::connect(b, &QToolButton::clicked, b, [page] { open(page); });
    return b;
}

void attachTopicsMenu(QMenu* menu)
{
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] {
        menu->clear();
        std::optional<Root> root = currentRoot();
        if (root)
        {
            // Top-level pages and the pages of a group heading, each group after a separator named by its heading.
            int groupDepth = -1;
            for (const SidebarEntry& e : readSidebar(root->dir / "docs"))
            {
                if (e.label && e.depth == 0)
                {
                    groupDepth = 1;
                    menu->addSection(qs(e.title));
                    continue;
                }
                if (e.depth == 0)
                    groupDepth = -1;
                if (!e.external && !e.label && !e.target.empty() && (e.depth == 0 || e.depth == groupDepth))
                {
                    std::string page = e.target;
                    QAction* a = menu->addAction(qs(e.title), [page] { open(page); });
                    a->setToolTip("Shift+click opens it in the web browser");
                    a->setProperty(menuSearch::kSameAsProperty, QString::fromStdString(docs::pageActionId(page)));
                }
            }
        }
        if (menu->isEmpty())
            menu->addAction("(no docs yet: Help > Update Docs)")->setEnabled(false);
    });
}

void registerDocsUi()
{
    if (std::exchange(state().registered, true))
        return;
    panels::add({.id = kDocsDock,
                 .title = "Docs",
                 .create = [](QWidget* p) -> QWidget* { return new DocsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "book",
                 .open = false,
                 .toggleAction = "window.docs",
                 .floatFirst = true});
    actions::add({.id = "help.docs",
                  .label = "Lila Docs",
                  .category = "Help",
                  .run = [] { open(); },
                  .doc = "The user docs, readable offline: search, hotkeys and workflows. Shift+click opens them in the web browser.",
                  .python = "lila.docs.open()"});
    actions::add({.id = "help.updateDocs",
                  .label = "Update Docs",
                  .category = "Help",
                  .run = [] { requestUpdate(true); },
                  .doc = "Downloads the latest docs for this version of Lila now.",
                  .python = "lila.docs.update()"});

    using T = ArgType;
    ops::registerCommand({
        .name = "openDocs",
        .doc = "Show the Docs panel at a page: \"essentials\", \"curves?id=pipe\", \"python/\". Empty for the docs home. UI only.",
        .positionalDoc = "page",
        .flags = {{"page", "p", T::String, "", "Page id as in the docs site's URL after #/, optionally with ?id=<heading>"},
                  {"api", "a", T::String, "", "A Python symbol (lila.ops.polyCube) whose API page to open instead"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (std::string symbol = a.get<std::string>("api", ""); !symbol.empty())
            {
                if (!openApi(symbol, Route::Panel))
                    open("python/", Route::Panel);
                return nullptr;
            }
            open(pageArg(a), Route::Panel);
            return nullptr;
        },
    });
    ops::registerCommand({
        .name = "docsUpdate",
        .doc = "Download the latest docs for this version in the background; the result shows in the status line.",
        .undoable = false,
        .run = [](const Args&) -> json {
            requestUpdate(true);
            return nullptr;
        },
    });
    ops::registerCommand({
        .name = "docsInfo",
        .doc = "Where the docs are read from: {root, source (override | cache | bundled), ref, sha, refs, cacheRoot, autoUpdate}. "
               "root is null when there are none yet.",
        .undoable = false,
        .run = [](const Args&) -> json {
            Sources src = defaultSources();
            std::optional<Root> root = selectRoot(src);
            json out = {{"refs", src.refs},
                        {"cacheRoot", paths::toUtf8(src.cacheRoot)},
                        {"autoUpdate", PrefStore::get().value<bool>("general", "/docs/autoUpdate", true)},
                        {"root", nullptr}};
            if (root)
            {
                out["root"] = paths::toUtf8(root->dir);
                out["source"] = root->source;
                out["ref"] = root->ref;
                out["sha"] = root->sha;
            }
            return out;
        },
    });
}
} // namespace lila::ui::docs
