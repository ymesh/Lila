#include "ui/SceneDialogs.h"

#include "core/CrashHandler.h"
#include "core/CrashReport.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "io/Examples.h"
#include "io/SceneFile.h"
#include "io/SceneHistory.h"
#include "ui/SampleScenes.h"
#include "ui/AchievementRules.h"
#include "ui/Credits.h"
#include "ui/CreditsDialog.h"
#include "ui/DocsUi.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/IssueReport.h"
#include "ui/Telemetry.h"
#include "ui/Theme.h"
#include "ui/WelcomeNews.h"
#include "vcs/SourceControl.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QCheckBox>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui
{
namespace fs = std::filesystem;
namespace history = io::history;
using json = nlohmann::json;

namespace
{
constexpr int kPathRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kRevisionRole = Qt::UserRole + 2;
constexpr int kDescriptionRole = Qt::UserRole + 3;
constexpr int kSampleRole = Qt::UserRole + 5;
constexpr int kBadgeRole = Qt::UserRole + 6;
constexpr int kFolderRole = Qt::UserRole + 7;
constexpr int kFormatRole = Qt::UserRole + 8;
constexpr int kAgeRole = Qt::UserRole + 9;
/** Every card on the welcome page: examples, samples and news. */
constexpr qreal kCardRadius = 6;

/** pix cropped to fill size with rounded corners; a flat tile when pix is null. */
QPixmap roundedThumb(const QPixmap& pix, QSize size, qreal radius, qreal dpr)
{
    QPixmap out(size * dpr);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size.width(), size.height()), radius, radius);
    p.setClipPath(clip);
    const QRectF target(0, 0, size.width(), size.height());
    if (pix.isNull())
        p.fillRect(target, themeColor("altBase"));
    else
    {
        const qreal f = std::max(target.width() / pix.width(), target.height() / pix.height());
        const QSizeF crop(target.width() / f, target.height() / f);
        p.drawPixmap(target, pix, QRectF(QPointF((pix.width() - crop.width()) / 2, (pix.height() - crop.height()) / 2), crop));
    }
    return out;
}

/** An example or sample as a card: thumbnail, title and the start of its description, with a badge on samples to download. */
class ExampleCard : public QStyledItemDelegate
{
public:
    static constexpr QSize kSize{240, 210};
    static constexpr int kThumbHeight = 135;

    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return kSize; }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(opt.rect).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool selected = opt.state & QStyle::State_Selected, hover = opt.state & QStyle::State_MouseOver;
        QPainterPath shape;
        shape.addRoundedRect(card, kCardRadius, kCardRadius);
        p->fillPath(shape, themeColor("base"));

        const QRect thumb(opt.rect.left(), opt.rect.top(), opt.rect.width(), kThumbHeight);
        p->save();
        p->setClipPath(shape);
        const QPixmap pix = index.data(Qt::DecorationRole).value<QPixmap>();
        if (pix.isNull())
            p->fillRect(thumb, themeColor("altBase"));
        else
        {
            const QPixmap scaled = pix.scaled(thumb.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p->drawPixmap(thumb, scaled, QRect((scaled.width() - thumb.width()) / 2, (scaled.height() - thumb.height()) / 2, thumb.width(), thumb.height()));
        }
        p->restore();

        if (const QString badge = index.data(kBadgeRole).toString(); !badge.isEmpty())
        {
            QFont f = opt.font;
            f.setPointSizeF(f.pointSizeF() * 0.85);
            f.setBold(true);
            const QFontMetrics bm(f);
            const qreal w = bm.horizontalAdvance(badge) + 14, h = bm.height() + 6;
            const QRectF pill(thumb.right() - 8 - w, thumb.top() + 8, w, h);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 170));
            p->drawRoundedRect(pill, h / 2, h / 2);
            p->setFont(f);
            p->setPen(Qt::white);
            p->drawText(pill, Qt::AlignCenter, badge);
        }

        QRect text = opt.rect.adjusted(10, kThumbHeight + 8, -10, -8);
        QFont title = opt.font;
        title.setBold(true);
        p->setFont(title);
        p->setPen(themeColor("text"));
        const int titleHeight = p->fontMetrics().height();
        p->drawText(text.left(), text.top(), text.width(), titleHeight, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, text.width()));
        text.setTop(text.top() + titleHeight + 2);
        p->setFont(opt.font);
        p->setPen(themeColor("dimText"));
        // Whole lines only, with the last one elided, so the text never runs past the card.
        const QFontMetrics fm = p->fontMetrics();
        const int lines = std::max(1, text.height() / fm.lineSpacing());
        QString rest = index.data(kDescriptionRole).toString();
        for (int i = 0; i < lines && !rest.isEmpty(); ++i)
        {
            QString line;
            if (i == lines - 1)
                line = fm.elidedText(rest, Qt::ElideRight, text.width());
            else
            {
                int n = rest.size();
                while (n > 0 && fm.horizontalAdvance(rest.left(n)) > text.width())
                    n = rest.lastIndexOf(' ', n - 1) > 0 ? rest.lastIndexOf(' ', n - 1) : n - 1;
                line = rest.left(std::max(n, 1));
            }
            p->drawText(text.left(), text.top() + i * fm.lineSpacing(), text.width(), fm.lineSpacing(), Qt::AlignLeft | Qt::AlignVCenter, line);
            rest = rest.mid(line.size()).trimmed();
        }

        p->setPen(QPen(themeColor(selected ? "highlight" : hover ? "hover" : "border"), selected ? 2.0 : 1.0));
        p->setBrush(Qt::NoBrush);
        p->drawPath(shape);
        p->restore();
    }
};

/** A recent scene in two lines: name and format on top, folder and age under it. */
class RecentRow : public QStyledItemDelegate
{
public:
    static constexpr int kHeight = 46;

    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const override { return {opt.rect.width(), kHeight}; }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect.adjusted(0, 1, 0, -1);
        const QRect text = r.adjusted(10, 5, -10, -5);
        if (index.data(kPathRole).toString().isEmpty())
        {
            p->setPen(themeColor("dimText"));
            p->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }
        const bool selected = opt.state & QStyle::State_Selected, hover = opt.state & QStyle::State_MouseOver;
        if (selected || hover)
        {
            QColor fill = themeColor(selected ? "highlight" : "hover");
            if (!selected)
                fill.setAlphaF(0.3f);
            p->setPen(Qt::NoPen);
            p->setBrush(fill);
            p->drawRoundedRect(QRectF(r), 4, 4);
        }
        const QColor main = themeColor(selected ? "highlightText" : "text");
        QColor dim = themeColor(selected ? "highlightText" : "dimText");
        if (selected)
            dim.setAlphaF(0.75f);

        const int half = text.height() / 2;
        const QRect top(text.left(), text.top(), text.width(), half);
        const QRect bottom(text.left(), text.top() + half, text.width(), text.height() - half);
        const QFontMetrics fm(opt.font);

        int badgeWidth = 0;
        if (const QString format = index.data(kFormatRole).toString(); !format.isEmpty())
        {
            QFont smallFont = opt.font;
            smallFont.setPointSizeF(opt.font.pointSizeF() * 0.8);
            const QFontMetrics sm(smallFont);
            badgeWidth = sm.horizontalAdvance(format) + 10;
            const QRectF pill(top.right() - badgeWidth, top.center().y() - (sm.height() + 2) / 2.0, badgeWidth, sm.height() + 2);
            p->setPen(QPen(dim, 1.0));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(pill.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            p->setFont(smallFont);
            p->drawText(pill, Qt::AlignCenter, format);
            badgeWidth += 8;
        }
        p->setFont(opt.font);
        p->setPen(main);
        p->drawText(top.adjusted(0, 0, -badgeWidth, 0), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, top.width() - badgeWidth));

        const QString age = index.data(kAgeRole).toString();
        const int ageWidth = age.isEmpty() ? 0 : fm.horizontalAdvance(age) + 12;
        p->setPen(dim);
        p->drawText(bottom, Qt::AlignRight | Qt::AlignVCenter, age);
        p->drawText(bottom.adjusted(0, 0, -ageWidth, 0), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(kFolderRole).toString(), Qt::ElideMiddle, bottom.width() - ageWidth));
        p->restore();
    }
};

/** The examples grid: each wheel notch glides one row, landing on a row boundary, at most two rows ahead of the view. */
class WholeRowList : public QListWidget
{
public:
    explicit WholeRowList(QWidget* parent) : QListWidget(parent)
    {
        mGlide.setDuration(160);
        mGlide.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&mGlide, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant& v) { verticalScrollBar()->setValue(v.toInt()); });
    }

protected:
    void updateGeometries() override
    {
        QListWidget::updateGeometries();
        verticalScrollBar()->setSingleStep(std::max(1, gridSize().height() / 3));
    }

    void wheelEvent(QWheelEvent* e) override
    {
        if (e->modifiers() != Qt::NoModifier || e->angleDelta().y() == 0)
            return QListWidget::wheelEvent(e);
        e->accept();
        QScrollBar* bar = verticalScrollBar();
        const int row = std::max(1, gridSize().height());
        if (mGlide.state() != QAbstractAnimation::Running)
            mTarget = bar->value();
        // Touchpads and free-spinning wheels send fractions of a notch; a row moves only per whole notch.
        if ((mNotches < 0) != (e->angleDelta().y() < 0))
            mNotches = 0;
        mNotches += e->angleDelta().y();
        const int steps = mNotches / 120;
        if (steps == 0)
            return;
        mNotches -= steps * 120;
        const int shown = int(std::lround(double(bar->value()) / row));
        const int next = std::clamp(int(std::lround(double(mTarget) / row)) - steps, shown - 2, shown + 2);
        const int target = std::clamp(next * row, bar->minimum(), bar->maximum());
        if (target == mTarget)
            return;
        mTarget = target;
        mGlide.stop();
        mGlide.setStartValue(bar->value());
        mGlide.setEndValue(target);
        mGlide.start();
    }

private:
    QVariantAnimation mGlide;
    int mTarget = 0;
    int mNotches = 0;
};

/** Holds the examples grid at a whole number of rows, so a card is never cut off at the bottom edge. */
class WholeRows : public QWidget
{
public:
    WholeRows(QListWidget* list, QWidget* parent) : QWidget(parent), mList(list) { mList->setParent(this); }

protected:
    void resizeEvent(QResizeEvent*) override
    {
        const int row = std::max(1, mList->gridSize().height());
        const int frame = 2 * mList->frameWidth();
        const int rows = std::max(1, (height() - frame) / row);
        mList->setGeometry(0, 0, width(), std::min(height(), rows * row + frame));
    }

private:
    QListWidget* mList;
};

QString qpath(const fs::path& p)
{
    return QString::fromStdWString(p.wstring());
}

QString when(int64_t unixSeconds)
{
    if (!unixSeconds)
        return "-";
    return QDateTime::fromSecsSinceEpoch(unixSeconds).toString("yyyy-MM-dd  hh:mm:ss");
}

/** "Just now", "5 minutes ago", "3 hours ago", "Yesterday", a weekday within the week, then "Oct 22" or "Oct 22, 2025". */
QString relativeWhen(const QDateTime& t, const QDateTime& now)
{
    const qint64 secs = t.secsTo(now);
    const qint64 days = t.date().daysTo(now.date());
    auto count = [](qint64 n, const char* unit) { return QString("%1 %2%3 ago").arg(n).arg(unit).arg(n == 1 ? "" : "s"); };
    if (secs < 60)
        return "Just now";
    if (days == 0)
        return secs < 3600 ? count(secs / 60, "minute") : count(secs / 3600, "hour");
    if (days == 1)
        return "Yesterday";
    if (days < 7)
        return QLocale().dayName(t.date().dayOfWeek());
    return QLocale().toString(t.date(), t.date().year() == now.date().year() ? "MMM d" : "MMM d, yyyy");
}

QString historyKindLabel(history::Kind k)
{
    switch (k)
    {
    case history::Kind::Autosave:
        return "Autosave";
    case history::Kind::Emergency:
        return "Emergency save";
    case history::Kind::Backup:
        return "Backup";
    case history::Kind::Increment:
        return "Increment";
    case history::Kind::Revision:
        return "Source control";
    }
    return {};
}

/** The recent scenes: one column of two-line rows, no header or grid lines. */
QTreeWidget* makeRecentList(QWidget* parent)
{
    auto* list = new QTreeWidget(parent);
    list->setObjectName("startupRecent");
    list->setColumnCount(1);
    list->setHeaderHidden(true);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->setItemDelegate(new RecentRow(list));
    list->setStyleSheet("QTreeWidget#startupRecent { background: transparent; }"
                        "QTreeWidget#startupRecent::item, QTreeWidget#startupRecent::item:selected,"
                        "QTreeWidget#startupRecent::item:hover { background: transparent; border: none; }");
    list->viewport()->setAutoFillBackground(false);
    return list;
}

QTreeWidget* makeList(QWidget* parent, const QStringList& headers)
{
    auto* list = new QTreeWidget(parent);
    list->setHeaderLabels(headers);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->header()->setStretchLastSection(true);
    return list;
}
} // namespace

void openCrashReport(const fs::path& dir)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(dir / "report.txt")));
}

void openCrashFolder(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(dir)));
}

void copyCrashReport(const fs::path& dir)
{
    std::string text = crash::readText(dir / "report.txt");
    QGuiApplication::clipboard()->setText(QString::fromUtf8(text.data(), qsizetype(text.size())));
}

RecoveryDialog::RecoveryDialog(const std::vector<io::recovery::Candidate>& candidates, const fs::path& crashReport, QWidget* parent)
    : QDialog(parent), mCandidates(candidates)
{
    setWindowTitle(crashReport.empty() ? "Recover Unsaved Work" : "Lila Crashed");
    auto* layout = new QVBoxLayout(this);

    if (!crashReport.empty())
    {
        std::string summary = crash::reportSummary(crashReport);
        const std::string prefix = "Lila crashed: ";
        if (summary.starts_with(prefix))
            summary = summary.substr(prefix.size());
        auto* title = new QLabel("<b>Lila crashed last time.</b>", this);
        layout->addWidget(title);
        auto* what = new QLabel(QString::fromStdString(summary), this);
        what->setWordWrap(true);
        what->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(what);
        auto* row = new QHBoxLayout();
        auto* open = new QPushButton("Open Crash Report", this);
        auto* folder = new QPushButton("Open Crash Folder", this);
        auto* copy = new QPushButton("Copy Crash Report", this);
        copy->setToolTip("Everything a developer needs to see what crashed, ready to paste into a bug report or a chat");
        auto* report = new QPushButton("Report Crash...", this);
        report->setToolTip("Sends this crash to the Lila issue tracker, with its report attached privately");
        auto* copied = new QLabel(this);
        row->addWidget(report);
        row->addWidget(open);
        row->addWidget(folder);
        row->addWidget(copy);
        row->addWidget(copied);
        row->addStretch(1);
        layout->addLayout(row);
        auto* queued = new QLabel("The report opens when you close this.", this);
        queued->hide();
        layout->addWidget(queued);
        connect(report, &QPushButton::clicked, this, [this, crashReport, report, queued] {
            auto* window = qobject_cast<QMainWindow*>(parentWidget());
            if (!window)
                return;
            issueReport::showCrash(window, crashReport);
            report->setEnabled(false);
            queued->show();
        });
        connect(open, &QPushButton::clicked, this, [crashReport] { openCrashReport(crashReport); });
        connect(folder, &QPushButton::clicked, this, [crashReport] { openCrashFolder(crashReport); });
        connect(copy, &QPushButton::clicked, this, [crashReport, copied] {
            copyCrashReport(crashReport);
            copied->setText("Copied.");
        });
        layout->addSpacing(8);
    }

    if (candidates.empty())
    {
        auto* buttons = new QHBoxLayout();
        auto* close = new QPushButton("Close", this);
        close->setDefault(true);
        buttons->addStretch(1);
        buttons->addWidget(close);
        layout->addLayout(buttons);
        connect(close, &QPushButton::clicked, this, [this] { finish(Choice::Discard); });
        resize(640, 0);
        return;
    }

    QString intro = candidates.size() == 1 ? "This scene has unsaved work:" : "These scenes have unsaved work:";
    if (crashReport.empty())
        intro = "Lila did not shut down cleanly. " + intro;
    layout->addWidget(new QLabel(intro, this));

    mList = makeList(this, {"Scene", "Folder", "Recovered from", "Recovered at", "Saved file at"});
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto& c = candidates[i];
        auto* item = new QTreeWidgetItem(mList);
        item->setText(0, c.scene.empty() ? QString("untitled") : qpath(c.scene.filename()));
        item->setText(1, c.scene.empty() ? QString() : qpath(c.scene.parent_path()));
        item->setText(2, historyKindLabel(c.kind));
        item->setText(3, when(c.fileTime));
        item->setText(4, c.savedTime ? when(c.savedTime) : QString("never saved"));
        item->setToolTip(2, qpath(c.file));
        item->setData(0, kPathRole, int(i));
    }
    for (int col = 0; col < 5; ++col)
        mList->resizeColumnToContents(col);
    mList->setCurrentItem(mList->topLevelItem(0));
    layout->addWidget(mList, 1);
    if (candidates.size() > 1)
        layout->addWidget(new QLabel("Lila opens one scene at a time. The others stay available under File > Versions.", this));

    auto* buttons = new QHBoxLayout();
    auto* discard = new QPushButton("Discard", this);
    discard->setToolTip("Keep the saved files as they are. The recovered versions stay under File > Recovered Scenes.");
    mOpenSaved = new QPushButton("Open Saved", this);
    mOpenSaved->setToolTip("Open the last saved version of the selected scene");
    auto* recover = new QPushButton("Recover", this);
    recover->setToolTip("Open the recovered version as unsaved changes to the scene; Save writes the real file");
    recover->setDefault(true);
    buttons->addWidget(discard);
    buttons->addStretch(1);
    buttons->addWidget(mOpenSaved);
    buttons->addWidget(recover);
    layout->addLayout(buttons);

    connect(discard, &QPushButton::clicked, this, [this] { finish(Choice::Discard); });
    connect(mOpenSaved, &QPushButton::clicked, this, [this] { finish(Choice::OpenSaved); });
    connect(recover, &QPushButton::clicked, this, [this] { finish(Choice::Recover); });
    connect(mList, &QTreeWidget::itemActivated, this, [this] { finish(Choice::Recover); }, Qt::QueuedConnection);
    connect(mList, &QTreeWidget::currentItemChanged, this, [this] { mOpenSaved->setEnabled(selected().savedTime != 0); });
    mOpenSaved->setEnabled(selected().savedTime != 0);
    resize(760, 300);
}

const io::recovery::Candidate& RecoveryDialog::selected() const
{
    QTreeWidgetItem* item = mList->currentItem();
    int i = item ? item->data(0, kPathRole).toInt() : 0;
    return mCandidates[size_t(std::clamp(i, 0, int(mCandidates.size()) - 1))];
}

void RecoveryDialog::finish(Choice c)
{
    mChoice = c;
    accept();
}

namespace
{
constexpr int kRecentWidth = 320;

/** The Essentials link: a solid accent button, the one coloured control on the welcome page. */
class EssentialsButton : public QPushButton
{
public:
    explicit EssentialsButton(QWidget* parent) : QPushButton(parent)
    {
        setObjectName("startupEssentials");
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        QColor lead = themeColor("highlightText");
        lead.setAlphaF(0.75f);
        setStyleSheet(QString("QLabel { background: transparent; color: %1; }"
                              "QLabel#essentialsLead { font-size: 9pt; color: %2; }"
                              "QLabel#essentialsAction { font-size: 12pt; font-weight: 600; }")
                          .arg(themeColor("highlightText").name(), lead.name(QColor::HexArgb)));
        auto* text = new QVBoxLayout(this);
        text->setContentsMargins(20, 9, 20, 10);
        text->setSpacing(0);
        for (auto [label, name] : {std::pair{"New to Lila?", "essentialsLead"}, std::pair{"Read the Essentials  →", "essentialsAction"}})
        {
            auto* l = new QLabel(QString::fromUtf8(label), this);
            l->setObjectName(name);
            l->setAttribute(Qt::WA_TransparentForMouseEvents);
            text->addWidget(l);
        }
        setMinimumSize(text->sizeHint().expandedTo(QSize(240, 0)));
    }

protected:
    void enterEvent(QEnterEvent* e) override
    {
        QPushButton::enterEvent(e);
        update();
    }

    void leaveEvent(QEvent* e) override
    {
        QPushButton::leaveEvent(e);
        update();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor fill = themeColor("highlight");
        if (isDown())
            fill = fill.darker(115);
        else if (underMouse())
            fill = fill.lighter(115);
        QPainterPath shape;
        shape.addRoundedRect(QRectF(rect()), 6, 6);
        p.fillPath(shape, fill);
    }
};

QFrame* makeSupportCard(QWidget* parent);

/** The welcome page's title row: the app icon and name on the left, the support card and Essentials button on the right. */
QWidget* makeWelcomeHeader(QWidget* parent, QPushButton*& essentials)
{
    auto* header = new QWidget(parent);
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(12);
    auto* icon = new QLabel(header);
    icon->setPixmap(qApp->windowIcon().pixmap(QSize(40, 40), header->devicePixelRatioF()));
    row->addWidget(icon);
    auto* title = new QLabel("Welcome to Lila", header);
    title->setObjectName("startupWelcome");
    row->addWidget(title);
    row->addStretch(1);
    essentials = new EssentialsButton(header);
    if (QFrame* support = makeSupportCard(header))
    {
        support->setFixedHeight(essentials->minimumHeight());
        row->addWidget(support);
    }
    row->addWidget(essentials);
    return header;
}

/**
 * A news item as a slim card: its 16:9 banner beside the title and text; a click opens its link. fresh marks an item the
 * user had not seen before this showing. With more than one item, arrows and a count in the corner step through them
 * (step gets -1 or +1); the card keeps one height throughout.
 */
class NewsCard : public QFrame
{
public:
    static constexpr QSize kBanner{192, 108};

    NewsCard(const news::Item& item, bool fresh, int index, int count, const std::function<void(int)>& step, QWidget* parent)
        : QFrame(parent), mUrl(QString::fromStdString(item.url))
    {
        setObjectName("newsCard");
        if (!mUrl.isEmpty())
        {
            setCursor(Qt::PointingHandCursor);
            setToolTip(mUrl);
        }
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(1, 1, 16, 1);
        row->setSpacing(16);
        auto* banner = new QLabel(this);
        banner->setFixedSize(kBanner);
        banner->setPixmap(roundedThumb(QPixmap(news::imageFile(item)), kBanner, kCardRadius - 1, devicePixelRatioF()));
        row->addWidget(banner, 0, Qt::AlignTop);
        mText = new QVBoxLayout;
        mText->setContentsMargins(0, 12, 0, 12);
        mText->setSpacing(3);
        row->addLayout(mText, 1);

        static const std::map<std::string, QString> kTags = {{"release", "Release"}, {"tutorial", "Tutorial"}};
        QStringList meta;
        if (auto it = kTags.find(item.type); it != kTags.end())
            meta << it->second;
        if (!item.date.empty())
            meta << QLocale().toString(QDate::fromString(QString::fromStdString(item.date), Qt::ISODate), QLocale::LongFormat);
        if (!item.author.empty())
            meta << QString::fromStdString(item.author);
        auto* top = new QHBoxLayout;
        top->setSpacing(2);
        mText->addLayout(top);
        if (fresh)
        {
            auto* label = new QLabel("New", this);
            label->setObjectName("newsFresh");
            top->addWidget(label);
            top->addSpacing(6);
        }
        auto* metaLabel = new QLabel(meta.join("  ·  "), this);
        metaLabel->setObjectName("newsMeta");
        top->addWidget(metaLabel, 1);
        if (count > 1)
        {
            auto arrow = [&](const char* name, const QString& tip, int by) {
                auto* b = new QToolButton(this);
                b->setIcon(icon(name));
                b->setToolTip(tip);
                b->setAutoRaise(true);
                b->setCursor(Qt::ArrowCursor);
                connect(b, &QToolButton::clicked, this, [step, by] { step(by); });
                return b;
            };
            QToolButton* prev = arrow("chevron-left", "Newer", -1);
            QToolButton* next = arrow("chevron-right", "Older", 1);
            prev->setEnabled(index > 0);
            next->setEnabled(index + 1 < count);
            auto* position = new QLabel(QString("%1 / %2").arg(index + 1).arg(count), this);
            position->setObjectName("newsMeta");
            top->addWidget(prev);
            top->addWidget(position);
            top->addWidget(next);
        }
        addText(QString::fromStdString(item.title), "newsTitle");
        addText(QString::fromStdString(item.body), "newsBody");
        mText->addStretch(1);
        setFixedHeight(kBanner.height() + 2);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && !mUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(mUrl));
    }

private:
    void addText(const QString& text, const char* name)
    {
        if (text.isEmpty())
            return;
        auto* label = new QLabel(text, this);
        label->setObjectName(name);
        label->setWordWrap(true);
        mText->addWidget(label);
    }

    QString mUrl;
    QVBoxLayout* mText = nullptr;
};

/**
 * A showcase entry: the art fills the card, with its title and artist over the bottom edge. A click opens the entry's
 * link, or the artist's page when it has none.
 */
class ShowcaseCard : public QWidget
{
public:
    static constexpr QSize kSize{260, NewsCard::kBanner.height() + 2};

    ShowcaseCard(const news::Item& item, const QPixmap& art, QWidget* parent)
        : QWidget(parent), mArt(art), mTitle(QString::fromStdString(item.title)),
          mUrl(QString::fromStdString(item.url.empty() ? item.authorUrl : item.url))
    {
        setObjectName("showcaseCard");
        setFixedSize(kSize);
        setAttribute(Qt::WA_Hover);
        mBy = item.author.empty() ? QString::fromStdString(item.body) : "by " + QString::fromStdString(item.author);
        if (!item.author.empty())
            setToolTip(QString::fromStdString(item.body));
        if (!mUrl.isEmpty())
            setCursor(Qt::PointingHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && !mUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(mUrl));
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.drawPixmap(0, 0, roundedThumb(mArt, size(), kCardRadius, devicePixelRatioF()));

        // A dark fade under the caption so white text reads over any art.
        QPainterPath shape;
        shape.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kCardRadius, kCardRadius);
        QLinearGradient fade(0, height() * 0.35, 0, height());
        fade.setColorAt(0, QColor(0, 0, 0, 0));
        fade.setColorAt(1, QColor(0, 0, 0, 200));
        p.fillPath(shape, fade);

        QFont badge = font();
        badge.setPointSizeF(badge.pointSizeF() * 0.85);
        badge.setBold(true);
        const QFontMetrics bm(badge);
        const qreal bw = bm.horizontalAdvance("Showcase") + 14, bh = bm.height() + 6;
        const QRectF pill(8, 8, bw, bh);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(pill, bh / 2, bh / 2);
        p.setFont(badge);
        p.setPen(Qt::white);
        p.drawText(pill, Qt::AlignCenter, "Showcase");

        const QRect text = rect().adjusted(12, 0, -12, -9);
        p.setFont(font());
        p.setPen(QColor(255, 255, 255, 200));
        const QFontMetrics sm = p.fontMetrics();
        p.drawText(text, Qt::AlignLeft | Qt::AlignBottom, sm.elidedText(mBy, Qt::ElideRight, text.width()));
        QFont title = font();
        title.setPointSizeF(title.pointSizeF() * 1.1);
        title.setBold(true);
        p.setFont(title);
        p.setPen(Qt::white);
        p.drawText(text.adjusted(0, 0, 0, -sm.height() - 1), Qt::AlignLeft | Qt::AlignBottom,
                   p.fontMetrics().elidedText(mTitle, Qt::ElideRight, text.width()));

        p.setPen(QPen(themeColor(underMouse() ? "hover" : "border"), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);
    }

private:
    QPixmap mArt;
    QString mTitle;
    QString mBy;
    QString mUrl;
};

/** A tabler icon drawn in a theme colour other than the icon tint, at size logical pixels. */
QPixmap tintedIcon(const QString& name, const QColor& color, int size, qreal dpr)
{
    QFile f(QString::fromStdWString((paths::resourcesDir() / "icons" / "tabler").wstring()) + "/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QSvgRenderer renderer(f.readAll().replace("currentColor", color.name().toLatin1()));
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p, QRectF(0, 0, pm.width(), pm.height()));
    pm.setDevicePixelRatio(dpr);
    return pm;
}

/** The donation card in the welcome header, beside Essentials. Null in the Steam build: Valve allows no off-Steam donation links there. */
QFrame* makeSupportCard(QWidget* parent)
{
    if (telemetry::installChannel(paths::executableDir()) == "steam")
        return nullptr;
    auto* card = new QFrame(parent);
    card->setObjectName("supportCard");
    auto* layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 0, 14, 0);
    layout->setSpacing(8);

    auto* heart = new QLabel(card);
    heart->setPixmap(tintedIcon("heart", themeColor("accent"), 16, card->devicePixelRatioF()));
    layout->addWidget(heart, 0, Qt::AlignVCenter);
    auto* pitch = new QLabel(QString("Lila is free. <span style=\"color: %1; font-weight: 600;\">Chip in?</span>").arg(themeColor("brightText").name()), card);
    pitch->setObjectName("supportLead");
    layout->addWidget(pitch, 0, Qt::AlignVCenter);
    layout->addSpacing(8);

    struct Link
    {
        const char* icon;
        const char* label;
        const char* url;
        const char* tip;
    };
    std::vector<Link> links = {
        {"coffee", "Ko-fi", "https://ko-fi.com/jaredtaylor", "One-off or monthly donation on Ko-fi"},
        {"brand-github", "Sponsors", "https://github.com/sponsors/Vaei/", "One-off or monthly sponsorship on GitHub Sponsors"},
        {"brand-steam", "Steam", "https://store.steampowered.com/app/5520720/", "Buy Lila on Steam: adds achievements that teach you what's new"},
    };

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    buttons->setAlignment(Qt::AlignVCenter);
    for (const Link& l : links)
    {
        auto* b = new QPushButton(icon(l.icon), l.label, card);
        b->setFixedHeight(30);
        b->setToolTip(QString("%1\n%2").arg(l.tip, l.url));
        b->setCursor(Qt::PointingHandCursor);
        const QUrl url(l.url);
        QObject::connect(b, &QPushButton::clicked, b, [url] { QDesktopServices::openUrl(url); });
        buttons->addWidget(b, 1);
    }
    layout->addLayout(buttons);
    return card;
}

/**
 * "Made possible by" with every Eternal and Mythic supporter and one Legendary, the next one each showing; a click opens
 * the credits. Null while there is no one to name.
 */
QLabel* makeMadePossibleBy(QWidget* dialog)
{
    const int rotation = PrefStore::get().value<int>("general", kCreditsRotationPref, 0);
    const std::vector<std::string> names = credits::madePossibleBy(credits::current(), rotation);
    if (names.empty())
        return nullptr;
    PrefStore::get().set("general", kCreditsRotationPref, rotation + 1);
    QStringList list;
    for (const std::string& n : names)
        list << QString::fromStdString(n).toHtmlEscaped();
    QString joined = list.size() > 1 ? list.mid(0, list.size() - 1).join(", ") + " and " + list.last() : list.first();
    // The whole line is one anchor so it clicks through to the credits like a link, without a link's colour.
    auto* label = new QLabel(QString("<a href=\"credits\" style=\"color: %1; text-decoration: none;\">Made possible by "
                                     "<span style=\"color: %2; font-weight: 600;\">%3</span></a>")
                                 .arg(themeColor("dimText").name(), themeColor("brightText").name(), joined), dialog);
    label->setObjectName("newsMeta");
    label->setToolTip("Open the credits");
    QObject::connect(label, &QLabel::linkActivated, dialog, [dialog] {
        QWidget* owner = dialog->parentWidget();
        showCredits(owner ? owner : dialog);
    });
    return label;
}

QLabel* makeHeading(QWidget* parent, const QString& text)
{
    auto* heading = new QLabel(text, parent);
    heading->setObjectName("startupHeading");
    return heading;
}

} // namespace

StartupDialog::StartupDialog(Hooks hooks, bool welcome, QWidget* parent) : QDialog(parent), mHooks(std::move(hooks)), mWelcome(welcome)
{
    setObjectName("startupDialog");
    // A tool window rather than a popup: popups close whenever the app loses focus.
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setStyleSheet(QString("QDialog#startupDialog { background: %1; border: 1px solid %2; }"
                          "QLabel#startupTitle { color: %3; font-size: 13pt; }"
                          "QLabel#startupWelcome { color: %4; font-size: 18pt; font-weight: 600; }"
                          "QLabel#startupHeading { color: %5; font-size: 10pt; font-weight: 600; }"
                          "QFrame#newsCard { background: %6; border: 1px solid %2; border-radius: 6px; }"
                          "QFrame#newsCard:hover { border-color: %7; }"
                          "QLabel#newsMeta, QLabel#newsBody { color: %5; }"
                          "QLabel#newsTitle { color: %4; font-size: 11pt; font-weight: 600; }"
                          "QLabel#newsFresh { color: %8; font-weight: 600; }"
                          "QFrame#supportCard { background: %6; border: 1px solid %2; border-radius: 6px; }"
                          "QLabel#supportLead { color: %5; font-size: 10pt; }"
                          "QTreeWidget#startupRecent, QListWidget#welcomeExamples { activate-on-singleclick: 1; }")
                      .arg(themeColor("window").name(), themeColor("border").name(), themeColor("text").name(), themeColor("brightText").name(),
                           themeColor("dimText").name(), themeColor("base").name(), themeColor("hover").name(), themeColor("accent").name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(welcome ? 20 : 16, welcome ? 16 : 14, welcome ? 20 : 16, 12);

    mList = makeRecentList(this);
    json recent = PrefStore::get().value("general", "/recentFiles", json::array());
    const QDateTime now = QDateTime::currentDateTime();
    std::error_code ec;
    for (const json& r : recent.is_array() ? recent : json::array())
    {
        if (!r.is_string())
            continue;
        fs::path p = fs::path(r.get<std::string>());
        if (!fs::is_regular_file(p, ec))
            continue;
        const int64_t modified = history::unixTime(p);
        auto* item = new QTreeWidgetItem(mList);
        item->setText(0, qpath(p.stem()));
        item->setData(0, kFolderRole, qpath(p.parent_path()));
        // Only other formats get a badge; a .lila is the usual case.
        if (const QString format = qpath(p.extension()).mid(1).toUpper(); format != "LILA")
            item->setData(0, kFormatRole, format);
        item->setData(0, kAgeRole, modified ? relativeWhen(QDateTime::fromSecsSinceEpoch(modified), now) : QString());
        item->setToolTip(0, qpath(p) + "\nModified " + when(modified));
        item->setData(0, kPathRole, qpath(p));
    }
    if (mList->topLevelItemCount() == 0)
    {
        auto* none = new QTreeWidgetItem(mList, {"No recent scenes yet"});
        none->setFlags(Qt::NoItemFlags);
    }
    else
        mList->setCurrentItem(mList->topLevelItem(0));

    auto* newScene = new QPushButton(icon("file-plus"), "New Scene", this);
    auto* browse = new QPushButton(icon("folder-open"), "Open...", this);
    const char* showPref = welcome ? kShowWelcomePref : kShowRecentPref;
    auto* show = new QCheckBox(welcome ? "Show welcome at startup" : "Show on startup", this);
    show->setChecked(PrefStore::get().value<bool>("general", showPref, true));
    connect(show, &QCheckBox::toggled, this, [showPref](bool on) { PrefStore::get().set("general", showPref, on); });

    if (welcome)
    {
        QPushButton* essentials = nullptr;
        layout->addWidget(makeWelcomeHeader(this, essentials));
        connect(essentials, &QPushButton::clicked, this, [this] {
            reject();
            docs::open("essentials");
        });
        layout->addSpacing(18);

        auto* body = new QHBoxLayout;
        body->setSpacing(24);
        layout->addLayout(body, 1);

        // Starting or reopening a scene, the things done most, down the left.
        auto* left = new QWidget(this);
        left->setFixedWidth(kRecentWidth);
        auto* leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(0, 0, 0, 0);
        leftLayout->setSpacing(8);
        auto* actions = new QHBoxLayout;
        actions->setSpacing(8);
        for (QPushButton* b : {newScene, browse})
        {
            b->setMinimumHeight(34);
            actions->addWidget(b, 1);
        }
        leftLayout->addLayout(actions);
        leftLayout->addSpacing(14);
        leftLayout->addWidget(makeHeading(left, "Recent"));
        leftLayout->addWidget(mList, 1);
        body->addWidget(left);

        auto* right = new QVBoxLayout;
        right->setSpacing(8);
        body->addLayout(right);
        mNews = new QWidget(this);
        mNewsBand = new QHBoxLayout(mNews);
        mNewsBand->setContentsMargins(0, 0, 0, 14);
        mNewsBand->setSpacing(12);
        right->addWidget(mNews);
        right->addWidget(makeHeading(this, "Examples"));

        mExamples = new WholeRowList(this);
        mExamples->setObjectName("welcomeExamples");
        mExamples->setViewMode(QListView::IconMode);
        mExamples->setResizeMode(QListView::Adjust);
        mExamples->setMovement(QListView::Static);
        mExamples->setWrapping(true);
        mExamples->setUniformItemSizes(true);
        mExamples->setSpacing(6);
        mExamples->setGridSize(ExampleCard::kSize + QSize(12, 12));
        mExamples->setFrameShape(QFrame::NoFrame);
        mExamples->viewport()->setAutoFillBackground(false);
        mExamples->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mExamples->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        mExamples->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        mExamples->setMouseTracking(true);
        mExamples->setItemDelegate(new ExampleCard(mExamples));
        mSampleList = io::samples::list();
        for (size_t i = 0; i < mSampleList.size(); ++i)
        {
            const io::samples::Sample& x = mSampleList[i];
            auto* item = new QListWidgetItem(mExamples);
            item->setText(QString::fromStdString(x.name));
            item->setData(kDescriptionRole, QString::fromStdString(x.description));
            item->setToolTip("<p>" + QString::fromStdString(x.description).toHtmlEscaped() + "</p><p>" +
                             QString::fromStdString(x.credit).toHtmlEscaped() + "</p>");
            item->setData(kKindRole, int(i));
            item->setData(kSampleRole, true);
            if (!io::samples::bundled(x) && !io::samples::installed(x))
                item->setData(kBadgeRole, "Download  " + QString::fromStdString(io::samples::formatSize(x.size)));
            if (!x.thumbnail.empty())
                item->setData(Qt::DecorationRole, QPixmap(qpath(x.thumbnail)));
        }
        for (const io::examples::Example& x : io::examples::list())
        {
            auto* item = new QListWidgetItem(mExamples);
            item->setText(QString::fromStdString(x.name));
            item->setData(kDescriptionRole, QString::fromStdString(x.description));
            item->setToolTip("<p>" + QString::fromStdString(x.description).toHtmlEscaped() + "</p>");
            item->setData(kKindRole, int(mExampleList.size()));
            if (!x.thumbnail.empty())
                item->setData(Qt::DecorationRole, QPixmap(qpath(x.thumbnail)));
            mExampleList.push_back(x);
        }
        mExamplesArea = new WholeRows(mExamples, this);
        mExamplesArea->setFixedWidth(examplesWidth(3));
        mNews->setFixedWidth(examplesWidth(3));
        right->addWidget(mExamplesArea, 1);

        showNews(news::cached());
        QPointer<StartupDialog> self = this;
        news::refresh([self](const std::vector<news::Item>& items) {
            if (self)
                self->showNews(items);
        });
        layout->addSpacing(10);
        auto* bottom = new QHBoxLayout;
        bottom->addWidget(show);
        bottom->addStretch(1);
        if (QLabel* thanks = makeMadePossibleBy(this))
            bottom->addWidget(thanks);
        layout->addLayout(bottom);
    }
    else
    {
        auto* title = new QLabel("Recent Scenes", this);
        title->setObjectName("startupTitle");
        layout->addWidget(title);
        layout->addWidget(mList, 1);
        auto* row = new QHBoxLayout();
        row->addWidget(show);
        row->addStretch(1);
        row->addWidget(newScene);
        row->addWidget(browse);
        layout->addLayout(row);
    }

    mList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mList, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) { showRecentMenu(at); });

    // Queued: opening a scene tears this dialog down, and Qt's mouse handler still uses the list after emitting.
    connect(mList, &QTreeWidget::itemActivated, this, [this] { openSelected(mList); }, Qt::QueuedConnection);
    if (mExamples)
    {
        connect(mExamples, &QListWidget::itemActivated, this, [this] { openExampleSelected(); }, Qt::QueuedConnection);
        mExamples->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(mExamples, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) { showExampleMenu(at); });
        // One selection across the two lists, so Enter opens what is highlighted.
        connect(mExamples, &QListWidget::itemPressed, mList, [this] { mList->setCurrentItem(nullptr); });
        connect(mList, &QTreeWidget::itemPressed, mExamples, [this] { mExamples->setCurrentItem(nullptr); });
    }
    connect(newScene, &QPushButton::clicked, this, [this] {
        auto create = mHooks.newScene;
        reject();
        if (create)
            create();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        auto open = mHooks.open;
        reject();
        if (open)
            open({});
    });
    resize(welcome ? QSize(20 + kRecentWidth + 24 + examplesWidth(3) + 20, 900) : QSize(560, 420));
    qApp->installEventFilter(this);
}

void StartupDialog::showNews(const std::vector<news::Item>& items)
{
    // What was seen before this dialog opened: items stay marked New for the whole showing.
    if (!mSeenLoaded)
    {
        mSeenBefore = news::seenIds();
        mSeenLoaded = true;
    }
    const std::string today = QDate::currentDate().toString(Qt::ISODate).toStdString();
    mNewsItems.clear();
    std::vector<news::Item> showcase;
    for (const news::Item& item : items)
        if (!news::isShowcase(item))
            mNewsItems.push_back(item);
        else if (news::running(item, today))
            showcase.push_back(item);
    mNewsShown = news::firstUnseen(mNewsItems, mSeenBefore);

    // The showcase entry stays put for the whole showing; the next one comes up next time.
    auto shown = std::ranges::find(showcase, mShowcaseId, &news::Item::id);
    if (shown == showcase.end())
    {
        const int pick = news::pickShowcase(showcase, PrefStore::get().value("general", news::kShowcaseViewsPref, json::object()));
        if (pick >= 0)
        {
            shown = showcase.begin() + pick;
            mShowcaseId = shown->id;
            news::countView(mShowcaseId);
        }
    }
    delete mShowcaseCard;
    if (shown != showcase.end())
        mShowcaseCard = new ShowcaseCard(*shown, QPixmap(news::imageFile(*shown)), mNews);
    else
        mShowcaseCard = makeShowcasePlaceholder();
    mNewsBand->addWidget(mShowcaseCard);
    showNewsItem();
}

QWidget* StartupDialog::makeShowcasePlaceholder()
{
    news::Item invite;
    invite.title = "Your art here";
    invite.body = "Made something in Lila? Send it in.";
    invite.url = "https://lila-3d.github.io/";
    QPixmap art;
    for (const io::examples::Example& x : mExampleList)
        if (x.name == "Trim Frieze" && !x.thumbnail.empty())
            art = QPixmap(qpath(x.thumbnail));
    return new ShowcaseCard(invite, art, mNews);
}

void StartupDialog::showNewsItem()
{
    delete mNewsCard;
    mNewsCard = nullptr;
    if (mNewsItems.empty())
        return;
    // Queued: the step comes from a button on the card being replaced.
    auto step = [this](int by) {
        QTimer::singleShot(0, this, [this, by] {
            mNewsShown = size_t(std::clamp(int(mNewsShown) + by, 0, int(mNewsItems.size()) - 1));
            showNewsItem();
        });
    };
    const news::Item& item = mNewsItems[mNewsShown];
    const bool fresh = std::ranges::find(mSeenBefore, item.id) == mSeenBefore.end();
    mNewsCard = new NewsCard(item, fresh, int(mNewsShown), int(mNewsItems.size()), step, mNews);
    mNewsBand->insertWidget(0, mNewsCard, 1);
    news::markSeen(item.id);
}

int StartupDialog::examplesWidth(int across) const
{
    return across * mExamples->gridSize().width() + 2 * mExamples->frameWidth() + 2 * mExamples->spacing() + 2 +
           mExamples->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, mExamples);
}

void StartupDialog::present()
{
    QWidget* anchor = parentWidget();
    QScreen* screen = anchor ? anchor->screen() : QGuiApplication::primaryScreen();
    QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1600, 900);
    // Never bigger than the screen: the page scrolls instead.
    resize(size().boundedTo(area.size() * 0.92));
    if (mExamplesArea)
    {
        // Ends at the examples' last whole row instead of leaving a band under them.
        layout()->activate();
        const int row = std::max(1, mExamples->gridSize().height());
        const int rows = mExamplesArea->height() - 2 * mExamples->frameWidth();
        if (rows > row)
            resize(width(), height() - rows % row);
    }
    move(area.center() - QPoint(width() / 2, height() / 2));
    show();
    raise();
    activateWindow();
    mList->setFocus();
    mReclaimFocus.start();
}

bool StartupDialog::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::KeyPress && isVisible() && qobject_cast<QWidget*>(obj) && static_cast<QWidget*>(obj)->window() == this)
    {
        auto* ke = static_cast<QKeyEvent*>(e);
        // The main window's shortcuts don't reach a tool window, and these two replace what the dialog is for.
        if (ke->matches(QKeySequence::Open) || ke->matches(QKeySequence::New))
        {
            auto run = ke->matches(QKeySequence::Open) ? std::function<void()>([open = mHooks.open] {
                if (open)
                    open({});
            })
                                                       : mHooks.newScene;
            reject();
            if (run)
                run();
            return true;
        }
        // The app's Esc hotkey (viewport.escape) would otherwise take it first.
        if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier)
        {
            reject();
            return true;
        }
        // Space opens the highlighted scene or example like Enter, instead of toggling the list selection.
        if (ke->key() == Qt::Key_Space && ke->modifiers() == Qt::NoModifier && (obj == mList || (mExamples && obj == mExamples)))
        {
            const bool example = obj == mExamples;
            if (example ? mExamples->currentItem() != nullptr : mList->currentItem() != nullptr)
                QTimer::singleShot(0, this, [this, example] { example ? openExampleSelected() : openSelected(mList); });
            return true;
        }
    }
    // The main window finishes activating after the dialog is shown at startup and would take the keyboard. Only then:
    // later activations are the user going back to the window.
    if (e->type() == QEvent::WindowActivate && obj == parentWidget() && isVisible() && mReclaimFocus.isValid() &&
        mReclaimFocus.elapsed() < 2000)
        QTimer::singleShot(0, this, [this] {
            activateWindow();
            mList->setFocus();
        });
    if (e->type() == QEvent::MouseButtonPress && isVisible() && !mWelcome)
    {
        auto* me = static_cast<QMouseEvent*>(e);
        auto* w = qobject_cast<QWidget*>(obj);
        if (w && w->window() != this && !qobject_cast<QMenu*>(w->window()) && !geometry().contains(me->globalPosition().toPoint()))
            reject();
    }
    return QDialog::eventFilter(obj, e);
}

void StartupDialog::openExampleSelected()
{
    QListWidgetItem* item = mExamples->currentItem();
    if (!item)
        return;
    if (item->data(kSampleRole).toBool())
    {
        openSampleSelected();
        return;
    }
    io::examples::Example x = mExampleList[size_t(item->data(kKindRole).toInt())];
    auto open = mHooks.openExample;
    accept();
    if (open)
        open(x);
}

void StartupDialog::openSampleSelected()
{
    QListWidgetItem* item = mExamples->currentItem();
    if (!item)
        return;
    const io::samples::Sample s = mSampleList[size_t(item->data(kKindRole).toInt())];
    // The details and the download sit over the welcome screen, which stays until the scene is ready to open.
    QPointer<StartupDialog> self = this;
    sampleScenes::fetch(this, s, mHooks.mayReplaceScene, [self, s] {
        auto open = self ? self->mHooks.openSample : nullptr;
        if (self)
            self->accept();
        if (open)
            open(s);
    });
    if (self && mExamples)
        mExamples->clearSelection();
}

void StartupDialog::showRecentMenu(const QPoint& at)
{
    QTreeWidgetItem* item = mList->itemAt(at);
    if (!item || item->data(0, kPathRole).toString().isEmpty())
        return;
    const QString path = item->data(0, kPathRole).toString();
    QMenu menu(this);
    menu.addAction(icon("folder-search"), "Open Containing Folder", [path] { fileOps::reveal(path); });
    menu.addAction(icon("x"), "Remove from Recents", [this, item] { removeRecent(item); });
    menu.exec(mList->viewport()->mapToGlobal(at));
}

void StartupDialog::showExampleMenu(const QPoint& at)
{
    QListWidgetItem* item = mExamples->itemAt(at);
    if (!item || !item->data(kSampleRole).toBool())
        return;
    const size_t index = size_t(item->data(kKindRole).toInt());
    const io::samples::Sample& s = mSampleList[index];
    const fs::path dir = io::samples::installDir(s);
    std::error_code ec;
    if (io::samples::bundled(s) || !fs::exists(dir, ec))
        return;

    QStringList sharing;
    for (const io::samples::Sample& other : mSampleList)
        if (other.archive == s.archive && other.id != s.id)
            sharing << QString::fromStdString(other.name);
    const QString what = sharing.isEmpty() ? QString("Move Download to Trash") : "Move Download to Trash (with " + sharing.join(", ") + ")";

    QMenu menu(this);
    menu.addAction(icon("folder-search"), "Open Containing Folder", [dir] { fileOps::reveal(qpath(dir)); });
    menu.addAction(icon("trash"), what, [this, index] { trashSample(index); });
    menu.exec(mExamples->viewport()->mapToGlobal(at));
}

void StartupDialog::trashSample(size_t index)
{
    const io::samples::Sample s = mSampleList[index];
    const QString dir = qpath(io::samples::installDir(s));
    if (!QFile::moveToTrash(dir))
    {
        QMessageBox::warning(this, "Move to Trash", "Couldn't move " + dir + " to the trash. A scene from it may still be open.");
        return;
    }
    for (int row = 0; row < mExamples->count(); ++row)
    {
        QListWidgetItem* item = mExamples->item(row);
        if (!item->data(kSampleRole).toBool())
            continue;
        const io::samples::Sample& x = mSampleList[size_t(item->data(kKindRole).toInt())];
        if (x.archive == s.archive)
            item->setData(kBadgeRole, "Download  " + QString::fromStdString(io::samples::formatSize(x.size)));
    }
}

void StartupDialog::removeRecent(QTreeWidgetItem* item)
{
    const std::string path = fs::path(item->data(0, kPathRole).toString().toStdWString()).generic_string();
    json recent = PrefStore::get().value("general", "/recentFiles", json::array());
    if (recent.is_array())
    {
        recent.erase(std::remove(recent.begin(), recent.end(), json(path)), recent.end());
        PrefStore::get().set("general", "/recentFiles", recent);
    }
    delete item;
}

void StartupDialog::openSelected(QTreeWidget* list)
{
    QTreeWidgetItem* item = list->currentItem();
    if (!item || item->data(0, kPathRole).toString().isEmpty())
        return;
    fs::path p = item->data(0, kPathRole).toString().toStdWString();
    if (list->indexOfTopLevelItem(item) == 0)
        achievementRules::lastSceneReopened();
    auto open = mHooks.open;
    accept();
    if (open)
        open(p);
}

VersionsDialog::VersionsDialog(const fs::path& scene, Hooks hooks, QWidget* parent)
    : QDialog(parent), mScene(scene), mHooks(std::move(hooks))
{
    setWindowTitle("Versions of " + QString::fromStdString(history::stemFor(scene)) +
                   (scene.empty() ? QString() : qpath(scene.extension())));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    mList = makeList(this, {"Kind", "Date", "Size", "Details"});
    mList->setSortingEnabled(false);
    layout->addWidget(mList, 1);

    auto* row = new QHBoxLayout();
    auto* note = new QLabel("Open loads a version as a new untitled scene. Restore replaces the current scene; "
                            "nothing is written until you save.",
                            this);
    note->setWordWrap(true);
    row->addWidget(note, 1);
    mOpen = new QPushButton("Open", this);
    mRestore = new QPushButton("Restore", this);
    auto* close = new QPushButton("Close", this);
    row->addWidget(mOpen);
    row->addWidget(mRestore);
    row->addWidget(close);
    layout->addLayout(row);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(mOpen, &QPushButton::clicked, this, [this] { act(false); });
    connect(mRestore, &QPushButton::clicked, this, [this] { act(true); });
    connect(mList, &QTreeWidget::itemActivated, this, [this] { act(false); }, Qt::QueuedConnection);
    auto refresh = [this] {
        QTreeWidgetItem* item = mList->currentItem();
        bool ok = item && item->data(0, kKindRole).isValid();
        mOpen->setEnabled(ok);
        mRestore->setEnabled(ok);
    };
    connect(mList, &QTreeWidget::currentItemChanged, this, refresh);
    populate();
    refresh();
    resize(820, 420);
}

void VersionsDialog::populate()
{
    for (const history::Version& v : history::list(mScene))
    {
        auto* item = new QTreeWidgetItem(mList);
        item->setText(0, historyKindLabel(v.kind));
        item->setText(1, when(v.time));
        item->setText(2, QLocale().formattedDataSize(qint64(v.size)));
        item->setText(3, qpath(v.path.filename()));
        item->setToolTip(3, qpath(v.path));
        item->setData(0, kPathRole, qpath(v.path));
        item->setData(0, kKindRole, int(v.kind));
    }
    if (mList->topLevelItemCount())
        mList->setCurrentItem(mList->topLevelItem(0));
    for (int col = 0; col < 3; ++col)
        mList->resizeColumnToContents(col);

    if (mScene.empty())
        return;
    QPointer<VersionsDialog> self = this;
    fs::path scene = mScene;
    vcs::SourceControl::get().async(
        [scene]() -> json {
            auto provider = vcs::SourceControl::get().providerFor(scene.parent_path());
            json out = json::array();
            if (!provider)
                return out;
            for (const vcs::Revision& r : provider->history(scene, 50))
                out.push_back(r.toJson());
            return out;
        },
        [self](const vcs::AsyncResult& r) {
            if (!self)
                return;
            if (r.ok)
                self->addRevisions(r.value);
            else
                log::warn("versions: {}", r.error);
        });
}

void VersionsDialog::addRevisions(const json& revisions)
{
    for (const json& r : revisions)
    {
        auto* item = new QTreeWidgetItem(mList);
        std::string rev = r.value("revision", "");
        std::string change = r.value("change", "");
        std::string desc = r.value("description", "");
        if (size_t nl = desc.find('\n'); nl != std::string::npos)
            desc.resize(nl);
        item->setText(0, historyKindLabel(history::Kind::Revision));
        item->setText(1, when(r.value("time", int64_t(0))));
        item->setText(3, QString::fromStdString((change.empty() ? "#" + rev : change) + "  " + r.value("user", "") + ": " + desc));
        item->setData(0, kKindRole, int(history::Kind::Revision));
        item->setData(0, kRevisionRole, QString::fromStdString(rev));
    }
    if (!mList->currentItem() && mList->topLevelItemCount())
        mList->setCurrentItem(mList->topLevelItem(0));
}

void VersionsDialog::act(bool restore)
{
    QTreeWidgetItem* item = mList->currentItem();
    if (!item || !item->data(0, kKindRole).isValid())
        return;
    auto kind = history::Kind(item->data(0, kKindRole).toInt());
    QString label = historyKindLabel(kind).toLower() + " " + item->text(1).simplified();
    if (restore)
    {
        QString text = "Replace the current scene with the " + label + "?\n\nThe scene file is only changed when you save; "
                       "reopen it to go back.";
        if (io::modified())
            text += "\n\nUnsaved changes to the current scene will be lost.";
        if (QMessageBox::question(this, "Restore Version", text) != QMessageBox::Yes)
            return;
    }
    else if (mHooks.confirmDiscard && !mHooks.confirmDiscard())
        return;

    if (kind != history::Kind::Revision)
    {
        load(fs::path(item->data(0, kPathRole).toString().toStdWString()), restore, label);
        return;
    }
    // Source control revisions are fetched to a cache next to the autosaves first.
    std::string rev = item->data(0, kRevisionRole).toString().toStdString();
    fs::path scene = mScene;
    std::string safeRev = rev.substr(0, 12);
    fs::path dest = history::baseDir("versions") / (scene.stem().string() + "." + safeRev + scene.extension().string());
    QPointer<VersionsDialog> self = this;
    vcs::SourceControl::get().async(
        [scene, rev, dest]() -> json {
            auto provider = vcs::SourceControl::get().providerFor(scene.parent_path());
            if (!provider)
                throw vcs::VcsError("the scene is no longer under source control");
            provider->fetchRevision(scene, rev, dest);
            return dest.string();
        },
        [self, restore, label, dest](const vcs::AsyncResult& r) {
            if (!self)
                return;
            if (!r.ok)
            {
                QMessageBox::warning(self, "Versions", QString::fromStdString(r.error));
                return;
            }
            self->load(dest, restore, label);
        });
}

void VersionsDialog::load(const fs::path& file, bool restore, const QString& label)
{
    try
    {
        std::string title = history::stemFor(mScene) + (mScene.empty() ? "" : mScene.extension().string()) + " (" +
                            label.toStdString() + ")";
        io::loadVersion(file, mScene, restore, title);
        if (mHooks.opened)
            mHooks.opened();
        accept();
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(this, "Versions", QString::fromStdString(e.what()));
    }
}
} // namespace lila::ui
