#include "ui/ChannelBox.h"
#include "ui/AttrAuthoring.h"
#include "ui/AttrView.h"
#include "ui/ChoiceTips.h"
#include "ui/Controls.h"
#include "ui/DrivePopover.h"
#include "ui/Tools.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QFormLayout>
#include <QMimeData>
#include <QSpinBox>
#include "ui/Panels.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "scene/RotationCurve.h"
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QResizeEvent>
#include <QPainter>
#include <QPointer>
#include <QToolButton>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/Spaces.h"
#include "ui/Animation.h"
#include "ui/Deletion.h"
#include "ui/NodeLookup.h"
#include "ui/NumberExpr.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/Tools.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QKeyEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui
{
namespace
{
using scene::Graph;

/** A name cell's tooltip parts: the attr and its doc, and the dim hints under them. */
constexpr int kTipBodyRole = Qt::UserRole + 1;
constexpr int kTipHintRole = Qt::UserRole + 2;

/** The selection's names, wrapped to the width and sized to fit up to kMaxRows rows; a longer selection scrolls. */
class SelectionStrip : public QListWidget
{
public:
    explicit SelectionStrip(QWidget* parent)
        : QListWidget(parent)
    {
        setFlow(QListView::LeftToRight);
        setWrapping(true);
        setResizeMode(QListView::Adjust);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSpacing(2);
    }

    void fitHeight()
    {
        doItemsLayout();
        constexpr int kMaxRows = 3;
        const int rowStep = count() ? sizeHintForRow(0) + 2 * spacing() : fontMetrics().height() + 10;
        int bottom = 0;
        for (int i = 0; i < count(); ++i)
            bottom = std::max(bottom, visualItemRect(item(i)).bottom() + 1 + verticalOffset());
        const int full = std::max(bottom + spacing(), fontMetrics().height() + 10);
        const int capped = std::min(full, kMaxRows * rowStep + spacing());
        setVerticalScrollBarPolicy(full > capped ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        setFixedHeight(capped + 2 * frameWidth());
    }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QListWidget::resizeEvent(e);
        if (e->size().width() != e->oldSize().width())
            QTimer::singleShot(0, this, [this] { fitHeight(); });
    }
};

/** The plug "Set as Driver" picked, for Drive From. */
std::string gDriverPlug;

double currentNumber(Graph& g, const scene::PlugRef& p)
{
    scene::PlugValue v = g.value(p);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

/** An enum channel's choices: the node's spaces for its space channel, else the attr's names. */
std::vector<std::string> enumChoices(const Graph& g, scene::NodeId owner, int attrIndex)
{
    if (attrIndex == scene::Transform::kSpace && scene::spaces::hasSpaces(g, owner))
        return scene::spaces::names(g, owner);
    const scene::Node* n = g.node(owner);
    return n && attrIndex >= 0 && attrIndex < int(n->type().attrs.size()) ? n->type().attrs[size_t(attrIndex)].enumNames
                                                                           : std::vector<std::string>();
}

/** What each of enumChoices means; spaces have no docs. */
std::vector<std::string> enumChoiceDocs(const Graph& g, scene::NodeId owner, int attrIndex)
{
    if (attrIndex == scene::Transform::kSpace && scene::spaces::hasSpaces(g, owner))
        return {};
    const scene::Node* n = g.node(owner);
    return n && attrIndex >= 0 && attrIndex < int(n->type().attrs.size()) ? choiceTips::forAttr(n->type().attrs[size_t(attrIndex)])
                                                                           : std::vector<std::string>();
}

/**
 * Enum values edit through a dropdown of their choices, which writes the chosen name back as the cell's text; the
 * cell shows a chevron so it reads as one. Other cells keep the line edit.
 */
class EnumDelegate : public QStyledItemDelegate
{
public:
    EnumDelegate(std::function<QStringList(int row)> choices, std::function<std::vector<std::string>(int row)> docs, QObject* parent)
        : QStyledItemDelegate(parent), mChoices(std::move(choices)), mDocs(std::move(docs))
    {
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStringList choices = mChoices(index.row());
        if (choices.isEmpty())
        {
            QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
            if (auto* line = qobject_cast<QLineEdit*>(editor))
                line->setAlignment(Qt::Alignment(index.data(Qt::TextAlignmentRole).toInt()) & Qt::AlignHorizontal_Mask);
            return editor;
        }
        auto* combo = new QComboBox(parent);
        combo->addItems(choices);
        choiceTips::apply(combo, mDocs(index.row()));
        connect(combo, &QComboBox::activated, combo, [this, combo] {
            auto* self = const_cast<EnumDelegate*>(this);
            Q_EMIT self->commitData(combo);
            Q_EMIT self->closeEditor(combo);
        });
        QTimer::singleShot(0, combo, [combo] { combo->showPopup(); });
        return combo;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        if (auto* combo = qobject_cast<QComboBox*>(editor))
            combo->setCurrentIndex(std::max(0, combo->findText(index.data(Qt::DisplayRole).toString())));
        else
            QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        if (auto* combo = qobject_cast<QComboBox*>(editor))
            model->setData(index, combo->currentText());
        else
            QStyledItemDelegate::setModelData(editor, model, index);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        // Editable values sit in a field like the Attribute Editor's, so they read as something to drag or type into.
        QStyleOptionViewItem opt(option);
        if (index.flags() & Qt::ItemIsEditable)
        {
            QVariant bg = index.data(Qt::BackgroundRole);
            // The table is drawn on the field colour itself, so the field steps up from it.
            QColor fill = bg.isValid() ? bg.value<QBrush>().color() : option.palette.color(QPalette::AlternateBase);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(option.palette.color(QPalette::Button), 1.0));
            painter->setBrush(fill);
            painter->drawRoundedRect(QRectF(option.rect).adjusted(1.5, 1.5, -1.5, -1.5), 2.0, 2.0);
            painter->restore();
            opt.rect.adjust(4, 0, -5, 0);
        }
        QStyledItemDelegate::paint(painter, opt, index);
        if (mChoices(index.row()).isEmpty())
            return;
        const QRectF r = option.rect;
        const double cx = r.right() - 7.0, cy = r.center().y() + 0.5;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QColor c = index.data(Qt::ForegroundRole).value<QBrush>().color();
        if (!index.data(Qt::ForegroundRole).isValid())
            c = option.palette.color(QPalette::Text);
        painter->setPen(QPen(c, 1.2));
        painter->drawPolyline(QPolygonF({QPointF(cx - 3.0, cy - 1.5), QPointF(cx, cy + 1.5), QPointF(cx + 3.0, cy - 1.5)}));
        painter->restore();
    }

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        // paint fills the field with the cell's background (keyed, locked), so the style must not fill the cell too.
        if (index.flags() & Qt::ItemIsEditable)
            option->backgroundBrush = Qt::NoBrush;
    }

private:
    std::function<QStringList(int row)> mChoices;
    std::function<std::vector<std::string>(int row)> mDocs;
};

QString formatNumber(double v)
{
    if (std::abs(v) < 5e-7)
        v = 0.0;
    QString s = QString::number(v, 'f', 3);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

double numericOf(const scene::PlugValue& v, int component)
{
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    if (auto* v3 = std::get_if<glm::dvec3>(&v); v3 && component >= 0)
        return (*v3)[component];
    return 0.0;
}

std::vector<ChannelBox*> gBoxes;
ChannelBox* gSliderBox = nullptr; // the box a viewport middle drag is scrubbing

/** One channel from Copy Selected Values. Input rows (polyCube1.width) remember their node type to find it again. */
struct CopiedValue
{
    std::string attr;
    std::string inputType;
    json value;
};
std::vector<CopiedValue> gCopied;
constexpr const char* kCbPlugMime = "application/x-lila-plug";
QString gCopiedText; // what the copy put on the clipboard; a different clipboard means someone else copied since

/** A value held inside the attr's hard limits. */
double clampToLimits(const scene::AttrDef& def, double v)
{
    if (def.minValue)
        v = std::max(v, *def.minValue);
    if (def.maxValue)
        v = std::min(v, *def.maxValue);
    return v;
}

/**
 * The setAttr value for text typed into a channel: on/off for bools, an enum's name or index, otherwise a number or
 * an expression on the channel's current value. nullopt when the text doesn't fit the channel.
 */
std::optional<json> parseValue(Graph& g, const std::string& plug, scene::PlugType type, const QString& text)
{
    if (type == scene::PlugType::Bool)
    {
        std::optional<bool> b = parseBool(text);
        return b ? std::optional<json>(*b) : std::nullopt;
    }
    if (type == scene::PlugType::Enum)
    {
        const scene::PlugRef ref = g.plug(plug);
        const auto& names = ref.attr == scene::Transform::kSpace && scene::spaces::hasSpaces(g, ref.node) ? scene::spaces::names(g, ref.node)
                                                                                                     : g.attr(ref).enumNames;
        auto it = std::find(names.begin(), names.end(), text.trimmed().toStdString());
        if (it != names.end())
            return json(int64_t(it - names.begin()));
        bool ok = false;
        int idx = text.toInt(&ok);
        return ok ? std::optional<json>(idx) : std::nullopt;
    }
    double current = numericOf(g.value(g.plug(plug)), -1);
    auto v = evalExpression(text, current);
    if (!v)
        return std::nullopt;
    *v = clampToLimits(g.attr(g.plug(plug)), *v);
    if (type == scene::PlugType::Int)
        return json(int64_t(std::llround(*v)));
    return json(*v);
}

/** How a channel's default reads in its tooltip, or empty when it has none. */
QString defaultText(const scene::AttrDef& def, int component)
{
    std::optional<json> v = valueReset::plugDefault(def, component);
    return v ? QString::fromStdString(valueReset::describe(*v, def.enumNames)) : QString();
}

bool hasPlug(Graph& g, const std::string& plug)
{
    try
    {
        g.plug(plug);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace

namespace channelSlider
{
bool begin(QPoint globalPos)
{
    const std::string when = attrView::viewportScrub();
    if (when == "off" || (when == "select" && tools::current() != Tool::Select))
        return false;
    for (ChannelBox* box : gBoxes)
    {
        if (!box->isVisible())
            continue;
        box->beginScrub(globalPos);
        if (box->mScrub)
        {
            gSliderBox = box;
            return true;
        }
    }
    return false;
}

void drag(QPoint globalPos, Qt::KeyboardModifiers mods)
{
    if (gSliderBox && gSliderBox->mScrub)
        gSliderBox->scrub(globalPos, mods);
}

void end()
{
    if (gSliderBox && gSliderBox->mScrub)
        gSliderBox->endScrub(gSliderBox->mScrub->moved);
    gSliderBox = nullptr;
}

bool active() { return gSliderBox != nullptr; }
} // namespace channelSlider

static QIcon cbCrossedIcon(const QString& name);

/** Room left of a channel's label for its state: the keyed bar and a 12 px glyph. */
constexpr QSize kCbStateIconSize{20, 16};

ChannelBox::ChannelBox(QWidget* parent) : QWidget(parent)
{
    gBoxes.push_back(this);
    mClock.start();
    mScrubTimer = new QTimer(this);
    mScrubTimer->setSingleShot(true);
    connect(mScrubTimer, &QTimer::timeout, this, &ChannelBox::applyScrub);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    setProperty("lilaMultiEditPanel", true);
    attrView::installKeyWatch();
    mStrip = new SelectionStrip(this);
    mStrip->setObjectName("cbObjects");
    mStrip->setDragDropMode(QAbstractItemView::InternalMove);
    mStrip->setToolTip("The selection in order: click to make one the lead, drag to reorder");
    layout->addWidget(mStrip);
    connect(mStrip, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        attrView::setLead(scene::NodeId(item->data(Qt::UserRole).toULongLong()));
        scheduleRefresh();
    });
    connect(mStrip->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        json order = json::array();
        Graph& g = Graph::current();
        for (int i = 0; i < mStrip->count(); ++i)
            if (scene::Node* n = g.node(scene::NodeId(mStrip->item(i)->data(Qt::UserRole).toULongLong())))
                order.push_back(n->path());
        QTimer::singleShot(0, this, [order] {
            try
            {
                ops::execute("select", {{"args", order}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        });
    });
    mName = new QLineEdit(this);
    mName->setPlaceholderText("Nothing selected");
    mName->setFocusPolicy(Qt::ClickFocus);
    QFont f = mName->font();
    f.setWeight(QFont::DemiBold);
    mName->setFont(f);
    layout->addWidget(mName);
    connect(mName, &QLineEdit::editingFinished, this, [this] {
        scene::Node* n = Graph::current().node(mNode);
        if (!n || mName->text().toStdString() == n->name() || mName->text().isEmpty())
            return;
        try
        {
            ops::execute("rename", {{"args", {n->path(), mName->text().toStdString()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            scheduleRefresh();
        }
    });

    // Filter text and chips, the shared name mode and Graph Editor sync.
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(2);
    mFilter = new QLineEdit(this);
    mFilter->setObjectName("cbFilter");
    mFilter->setPlaceholderText("Filter channels");
    mFilter->setClearButtonEnabled(true);
    connect(mFilter, &QLineEdit::textChanged, this, [this] { scheduleRefresh(); });
    filterRow->addWidget(mFilter, 1);
    filterRow->addSpacing(6);
    mNameMode = new QToolButton(this);
    mNameMode->setObjectName("cbNameMode");
    mNameMode->setAutoRaise(true);
    mNameMode->setIcon(icon("letter-case"));
    mNameMode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mNameMode->setToolTip("<b>Channel Names</b><br>Display (Translate X), long (translateX) or short (tx). Click to cycle, "
                          "right-click to go back.<br>Shared with the Attribute Editor.");
    controls::cycle(mNameMode, [](int step) {
        static constexpr attrView::NameMode kOrder[] = {attrView::NameMode::Nice, attrView::NameMode::Long, attrView::NameMode::Short};
        const int at = int(std::find(std::begin(kOrder), std::end(kOrder), attrView::nameMode()) - std::begin(kOrder));
        attrView::setNameMode(kOrder[(at + step + 3) % 3]);
    });
    filterRow->addWidget(mNameMode);
    mSync = new QToolButton(this);
    mSync->setObjectName("cbSyncGraph");
    mSync->setIcon(icon("chart-line"));
    mSync->setAutoRaise(true);
    mSync->setCheckable(true);
    mSync->setChecked(attrView::syncGraphEditor());
    mSync->setToolTip("<b>Sync Graph Editor</b><br>Show only the highlighted channels in the Graph Editor.");
    connect(mSync, &QToolButton::toggled, this, [](bool on) { attrView::setSyncGraphEditor(on); });
    filterRow->addWidget(mSync);
    auto* settings = new QToolButton(this);
    settings->setObjectName("cbSettings");
    settings->setIcon(icon("settings"));
    settings->setAutoRaise(true);
    settings->setToolTip("<b>Channel Box Settings</b><br>Scrub speed, hyperbolic scrub, viewport middle drag, precision, namespaces.");
    connect(settings, &QToolButton::clicked, this, [this, settings] { showSettings(settings); });
    filterRow->addWidget(settings);
    layout->addLayout(filterRow);
    mChipRow = new QWidget(this);
    auto* chips = new FlowLayout(2);
    mChipRow->setLayout(chips);
    struct ChipDef
    {
        const char* name;
        const char* what;
        const char* on;
        const char* off;
    };
    static const ChipDef kChips[] = {
        {"Keyed", "keyed", "key", "key-off"},
        {"Driven", "driven", "steering-wheel", "steering-wheel-off"},
        {"Connected", "connected", "plug-connected", "plug-connected-x"},
        {"User", "user", "user", "user-off"},
        {"Locked", "locked", "lock", "lock-off"},
        {"TRS", "translate, rotate and scale", "arrows-move", nullptr},
        {"Type", "the lead's own type's", "cube", "cube-off"},
    };
    for (const ChipDef& chip : kChips)
    {
        auto* b = new QToolButton(mChipRow);
        b->setObjectName(QString("cbChip_") + chip.name);
        b->setIcon(icon(chip.on));
        b->setAutoRaise(true);
        b->setCheckable(true);
        const auto tip = [&chip](int s) {
            const QString what = QString(chip.what);
            const QString state = s == 1 ? "Showing only " + what + " channels."
                                  : s == 2 ? "Hiding " + what + " channels."
                                           : "Off.";
            return QString("<b>%1</b><br>%2<br>Click to cycle: only %3, all but %3, off. Right-click to go back.").arg(chip.name, state, what);
        };
        b->setToolTip(tip(0));
        mChips[chip.name] = 0;
        controls::cycle(b, [this, b, chip, tip](int step) {
            int& s = mChips[chip.name];
            s = (s + step + 3) % 3;
            b->setChecked(s != 0);
            b->setIcon(s != 2 ? icon(chip.on) : chip.off ? icon(chip.off) : cbCrossedIcon(chip.on));
            b->setToolTip(tip(s));
            scheduleRefresh();
        });
        // State chips, then the kind chips (TRS, Type) after a gap.
        if (std::string_view(chip.name) == "TRS")
        {
            auto* gap = new QWidget(mChipRow);
            gap->setFixedSize(8, 1);
            chips->addWidget(gap);
        }
        chips->addWidget(b);
    }
    auto* chipLine = new QHBoxLayout;
    chipLine->setSpacing(6);
    auto* showLabel = new QLabel("Show", this);
    showLabel->setObjectName("cbChipLabel");
    showLabel->setToolTip("Filter channels by state or kind. Each chip cycles: only, all but, off.");
    QPalette dimPal = showLabel->palette();
    dimPal.setColor(QPalette::WindowText, themeColor("dimText"));
    showLabel->setPalette(dimPal);
    showLabel->setContentsMargins(2, 4, 0, 0);
    chipLine->addWidget(showLabel, 0, Qt::AlignTop);
    chipLine->addWidget(mChipRow, 1);
    layout->addLayout(chipLine);
    mHint = new QFrame(this);
    mHint->setObjectName("cbHint");
    auto* hintRow = new QHBoxLayout(mHint);
    hintRow->setContentsMargins(6, 3, 6, 3);
    mHintText = new QLabel(mHint);
    mHintText->setObjectName("cbHintText");
    mHintText->setWordWrap(true);
    mHintText->setPalette(dimPal);
    auto* hintOff = new QLabel("<a href=\"off\">Turn off this hint</a>", mHint);
    connect(hintOff, &QLabel::linkActivated, this, [this] {
        attrView::setHintEnabled(false);
        mHint->hide();
    });
    hintRow->addWidget(mHintText, 1);
    hintRow->addWidget(hintOff);
    mHint->hide();
    layout->addWidget(mHint);
    auto showMode = [this] { mNameMode->setText(attrView::nameMode() == attrView::NameMode::Nice ? "Display"
                                                : attrView::nameMode() == attrView::NameMode::Long ? "Long" : "Short"); };
    showMode();
    mViewListener = attrView::addListener([this, showMode, mode = attrView::nameMode(), places = attrView::precision()]() mutable {
        showMode();
        // A name mode change relabels every row; the Alt key and the lead only need values.
        if (mode != attrView::nameMode() || places != attrView::precision())
        {
            mode = attrView::nameMode();
            places = attrView::precision();
            mRows.clear();
        }
        scheduleRefresh();
    });

    mTable = new QTableWidget(0, 2, this);
    // The rows are the scene selection's channels, so Delete removes the selection as in Maya.
    deletion::claimScene(this);
    mTable->horizontalHeader()->hide();
    mTable->verticalHeader()->hide();
    mTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    mTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    mTable->setColumnWidth(1, 90);
    // Room for the longer channel names (Key Tolerance, morph targets) beside a full value column.
    mTable->setMinimumWidth(12 * mTable->fontMetrics().height() + 90 + 2 * mTable->frameWidth());
    mTable->verticalHeader()->setDefaultSectionSize(20);
    mTable->setShowGrid(false);
    mTable->setIconSize(kCbStateIconSize);
    mTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    mTable->setProperty("numericEntry", true);
    mTable->viewport()->installEventFilter(this);
    mTable->viewport()->setAcceptDrops(true);
    mTable->installEventFilter(this);
    layout->addWidget(mTable, 1);
    connect(mTable, &QTableWidget::itemSelectionChanged, this, [this] {
        std::vector<scene::PlugRef> plugs;
        for (int r : highlightedRows())
        {
            const Row& row = mRows[size_t(r)];
            plugs.push_back({row.node ? row.node : mNode, row.attrIndex, row.component, row.element});
        }
        attrView::setHighlightedChannels(plugs);
    });
    connect(mTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!mUpdating)
            commitCell(item);
    });
    mTable->setItemDelegateForColumn(1, new EnumDelegate(
                                            [this](int r) {
                                                QStringList out;
                                                if (r < 0 || r >= int(mRows.size()) || mRows[r].header || mRows[r].type != scene::PlugType::Enum)
                                                    return out;
                                                for (const std::string& c : enumChoices(Graph::current(), mRows[r].node ? mRows[r].node : mNode, mRows[r].attrIndex))
                                                    out << QString::fromStdString(c);
                                                return out;
                                            },
                                            [this](int r) {
                                                if (r < 0 || r >= int(mRows.size()) || mRows[r].header || mRows[r].type != scene::PlugType::Enum)
                                                    return std::vector<std::string>();
                                                return enumChoiceDocs(Graph::current(), mRows[r].node ? mRows[r].node : mNode, mRows[r].attrIndex);
                                            },
                                            mTable));

    mListener = Graph::current().addListener([this](scene::Change, scene::NodeId) { scheduleRefresh(); });
    onTimeChanged(this, [this] { scheduleRefresh(); });
    // Highlighted channels take every middle drag in the viewport, so picking a transform tool (even the current one) drops them.
    mToolListener = tools::addListener([this, tool = tools::current(), resets = tools::handleResets()]() mutable {
        if (tools::current() == tool && tools::handleResets() == resets)
            return;
        tool = tools::current();
        resets = tools::handleResets();
        if (!mScrub)
            mTable->clearSelection();
    });

    mTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mTable, &QTableWidget::customContextMenuRequested, this, [this](QPoint pos) {
        QModelIndex idx = mTable->indexAt(pos);
        if (!idx.isValid() || idx.column() != 0 || idx.row() >= int(mRows.size()) || mRows[idx.row()].header)
            return;
        if (!mTable->item(idx.row(), 0)->isSelected())
            mTable->selectRow(idx.row());
        json plugs = json::array();
        Graph& g = Graph::current();
        for (QTableWidgetItem* item : mTable->selectedItems())
        {
            if (item->column() != 0 || item->row() >= int(mRows.size()) || mRows[item->row()].header)
                continue;
            const Row& row = mRows[item->row()];
            for (const std::string& name : row.node ? std::vector<std::string>{g.node(row.node)->path()} : allTargets())
                if (hasPlug(g, name + "." + row.attr))
                    plugs.push_back(name + "." + row.attr);
        }
        auto runEach = [plugs](const char* label, auto fn) {
            UndoChunk chunk(label);
            for (const json& p : plugs)
            {
                try
                {
                    fn(p.get<std::string>());
                }
                catch (const std::exception& e)
                {
                    log::error("{}", e.what());
                }
            }
        };
        bool anyKeyed = false, anyDriven = false, anyLocked = false, anyUnlocked = false, anyMuted = false;
        std::vector<scene::NodeId> sources;
        for (const json& p : plugs)
        {
            scene::NodeId src = scene::kNoNode;
            const uint32_t st = stateOf(g.plug(p.get<std::string>()), &src);
            anyKeyed |= (st & kKeyed) != 0;
            anyMuted |= (st & kMuted) != 0;
            anyDriven |= (st & (kKeyed | kDrivenKey | kExpression | kConnected | kConstraint)) != 0;
            (st & kLocked ? anyLocked : anyUnlocked) = true;
            if (src != scene::kNoNode && !(st & kKeyed) && std::find(sources.begin(), sources.end(), src) == sources.end())
                sources.push_back(src);
        }
        QMenu menu(this);
        menu.addAction("Key Selected", [runEach] {
            runEach("Key Selected", [](const std::string& p) { ops::execute("setKeyframe", {{"args", {p}}}, CallSource::UI); });
        });
        menu.addAction("Breakdown Selected", [runEach] {
            runEach("Breakdown Selected", [](const std::string& p) {
                ops::execute("setKeyframe", {{"args", {p}}, {"breakdown", true}}, CallSource::UI);
            });
        });
        if (anyKeyed)
            menu.addAction(anyMuted ? "Unmute Selected" : "Mute Selected", [runEach, anyMuted] {
                runEach("Mute", [anyMuted](const std::string& p) { ops::execute("mute", {{"args", {p}}, {"disable", anyMuted}}, CallSource::UI); });
            });
        menu.addAction("Key All", [objects = allTargets()] {
            try
            {
                ops::execute("setKeyframe", {{"args", objects}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        });
        menu.addAction("Mute All", [objects = allTargets()] {
            try
            {
                ops::execute("mute", {{"args", objects}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        });
        menu.addSeparator();
        menu.addAction("Copy Selected Values", [this] { copyValues(); });
        if (!QApplication::clipboard()->text().trimmed().isEmpty())
            menu.addAction("Paste Values", [this] { pasteValues(); });
        std::vector<std::string> layers;
        for (scene::NodeId id : g.nodes())
            if (const scene::Node* l = g.node(id); l && l->type().isA("animLayer"))
                layers.push_back(l->path());
        if (!layers.empty())
        {
            QMenu* add = menu.addMenu("Add to Anim Layer");
            for (const std::string& layer : layers)
                add->addAction(QString::fromStdString(layer), [plugs, layer] {
                    try
                    {
                        ops::execute("addAnimLayerMembers", {{"args", plugs}, {"layer", layer}}, CallSource::UI);
                    }
                    catch (const std::exception& e)
                    {
                        log::error("{}", e.what());
                    }
                });
        }
        menu.addSeparator();

        QMenu* freeze = menu.addMenu("Freeze");
        for (auto [label, t, r, s] : {std::tuple{"Translate", true, false, false}, std::tuple{"Rotate", false, true, false},
                                      std::tuple{"Scale", false, false, true}, std::tuple{"All", true, true, true}})
            freeze->addAction(label, [objects = allTargets(), t, r, s] {
                try
                {
                    ops::execute("freezeTransforms", {{"args", objects}, {"translate", t}, {"rotate", r}, {"scale", s}}, CallSource::UI);
                }
                catch (const std::exception& e)
                {
                    log::error("{}", e.what());
                }
            });
        menu.addAction("Reset to Default", [this] { resetRows(highlightedRows()); });
        QMenu* more = &menu;
        if (anyDriven)
            more->addAction("Break Connections", [runEach] {
            runEach("Break Connections", [](const std::string& p) {
                Graph& g = Graph::current();
                scene::PlugRef ref = g.plug(p);
                // An anim curve goes with its connection, as in Maya.
                if (scene::anim::curveFor(g, ref))
                    ops::execute("cutKey", {{"args", {p}}, {"clear", true}}, CallSource::UI);
                else if (g.source(ref))
                    ops::execute("disconnectAttr", {{"args", {p}}}, CallSource::UI);
            });
        });
        if (anyKeyed)
            menu.addAction("Delete Keys", [runEach] {
                runEach("Delete Keys", [](const std::string& p) { ops::execute("cutKey", {{"args", {p}}, {"clear", true}}, CallSource::UI); });
            });
        if (sources.size() == 1)
            menu.addAction("Select Source", [path = g.node(sources.front())->path()] {
                try
                {
                    ops::execute("select", {{"args", {path}}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
        json added = json::array();
        for (const json& p : plugs)
            if (const scene::PlugRef ref = g.plug(p.get<std::string>()); ref.valid() && ref.component < 0 &&
                                                                              (g.node(ref.node)->type().attrs[size_t(ref.attr)].flags & scene::AttrUser))
                added.push_back(p);
        if (added.size() == 1)
        {
            const scene::PlugRef ref = g.plug(added[0].get<std::string>());
            const std::string attr = g.attr(ref).name;
            menu.addAction(icon("pencil"), "Edit Attribute...", [this, node = ref.node, attr] { attrAuthoring::showEdit({node}, attr, mTable); });
        }
        if (!added.empty())
        {
            menu.addAction("Hide Selected", [added] {
                try
                {
                    ops::execute("editAttr", {{"args", added}, {"keyable", false}, {"channelBox", false}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
            menu.addAction("Make Nonkeyable", [added] {
                try
                {
                    ops::execute("editAttr", {{"args", added}, {"keyable", false}, {"channelBox", true}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
        }
        if (!added.empty())
            menu.addAction("Delete Attributes", [added] {
                UndoChunk chunk("Delete Attributes");
                try
                {
                    ops::execute("deleteAttr", {{"args", added}}, CallSource::UI);
                }
                catch (const std::exception& e)
                {
                    log::error("{}", e.what());
                }
            });
        menu.addSeparator();
        if (plugs.size() == 1)
            menu.addAction(icon("link"), "Drive...", [this, p = plugs[0].get<std::string>()] { drivePopover::show(p, mTable); });
        if (plugs.size() == 1)
            menu.addAction("Set as Driver", [p = plugs[0].get<std::string>()] {
                gDriverPlug = p;
                log::info("driver: {}; right-click a channel and choose Drive From to drive it", p);
            });
        if (!gDriverPlug.empty())
            menu.addAction(QString::fromStdString("Drive From " + gDriverPlug), [runEach] {
                runEach("Drive From", [](const std::string& p) {
                    Graph& g = Graph::current();
                    const double d = currentNumber(g, g.plug(gDriverPlug)), v = currentNumber(g, g.plug(p));
                    ops::execute("driveChannel",
                                 {{"args", {p}}, {"driver", gDriverPlug}, {"keys", json::array({json::array({d, v}), json::array({d + 1.0, v + 1.0})})},
                                  {"force", true}},
                                 CallSource::UI);
                });
            });
        menu.addSeparator();
        for (bool lock : {true, false})
            if (lock ? anyUnlocked : anyLocked)
                menu.addAction(lock ? "Lock Selected" : "Unlock Selected", [runEach, lock] {
                runEach(lock ? "Lock Selected" : "Unlock Selected", [lock](const std::string& p) {
                    ops::execute("setAttr", {{"args", {p}}, {"lock", lock}}, CallSource::UI);
                });
            });
        if (Graph& sg = Graph::current(); sg.node(mNode) && scene::spaces::hasSpaces(sg, mNode))
        {
            menu.addSeparator();
            QMenu* spaces = menu.addMenu("Switch Space");
            const std::string node = sg.node(mNode)->path();
            const std::vector<std::string> names = scene::spaces::names(sg, mNode);
            const int now = scene::spaces::active(sg, mNode);
            for (int i = 0; i < int(names.size()); ++i)
            {
                QAction* act = spaces->addAction(QString::fromStdString(names[size_t(i)]), [node, i] {
                    try
                    {
                        ops::execute("switchSpace", {{"args", {node}}, {"space", i}}, CallSource::UI);
                    }
                    catch (const std::exception& e)
                    {
                        log::error("{}", e.what());
                    }
                });
                act->setCheckable(true);
                act->setChecked(i == now);
            }
        }
        menu.exec(mTable->viewport()->mapToGlobal(pos));
    });
    refresh();
}

ChannelBox::~ChannelBox()
{
    attrView::removeListener(mViewListener);
    if (mScrub)
        endScrub(false);
    if (gSliderBox == this)
        gSliderBox = nullptr;
    std::erase(gBoxes, this);
    Graph::current().removeListener(mListener);
    tools::removeListener(mToolListener);
}

void ChannelBox::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        refresh();
    });
}

std::vector<std::string> ChannelBox::targets() const
{
    if (attrView::channelBoxEditsAll() || attrView::multiEditHeld())
        return allTargets();
    scene::Node* n = Graph::current().node(mNode);
    return n ? std::vector<std::string>{n->path()} : std::vector<std::string>{};
}

std::vector<std::string> ChannelBox::allTargets() const
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : attrView::selectionLeadFirst())
        if (scene::Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

namespace
{
/** Maya's channel colours, so the meaning carries over: data colour, not chrome. */
QColor cbStateColor(uint32_t state)
{
    using CB = ChannelBox;
    if (state & CB::kMuted)
        return QColor(154, 123, 82);
    if (state & CB::kBreakdown)
        return QColor(91, 180, 107);
    if (state & CB::kKeyed)
        return QColor(205, 91, 91);
    if (state & CB::kDrivenKey)
        return QColor(91, 143, 214);
    if (state & CB::kExpression)
        return QColor(167, 123, 214);
    if (state & CB::kConstraint)
        return QColor(107, 176, 200);
    if (state & CB::kConnected)
        return QColor(214, 194, 91);
    if (state & CB::kLocked)
        return QColor(138, 138, 138);
    return {};
}
} // namespace

/** An icon with a slash through it, for a Tabler icon that has no -off variant. */
static QIcon cbCrossedIcon(const QString& name)
{
    const QIcon base = icon(name);
    QIcon out;
    for (const auto& [mode, colour] : {std::pair{QIcon::Normal, "iconNormal"}, std::pair{QIcon::Active, "iconActive"}})
    {
        QPixmap pm = base.pixmap(QSize(16, 16), 2.0, mode, QIcon::Off);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(QPen(Qt::transparent, 3.5));
        p.drawLine(QPointF(4, 4), QPointF(28, 28));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(themeColor(colour), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 4), QPointF(28, 28));
        p.end();
        out.addPixmap(pm, mode, QIcon::Off);
        out.addPixmap(pm, mode, QIcon::On);
    }
    return out;
}

/** The state's glyph, so it reads without its hue: keyed channels carry the edge bar instead. */
static const char* cbStateGlyph(uint32_t state)
{
    using CB = ChannelBox;
    if (state & CB::kMuted)
        return "volume-off";
    if (state & CB::kBreakdown)
        return "circle-half";
    if (state & CB::kKeyed)
        return nullptr;
    if (state & CB::kDrivenKey)
        return "steering-wheel";
    if (state & CB::kExpression)
        return "math-function";
    if (state & CB::kConstraint)
        return "link";
    if (state & CB::kConnected)
        return "plug-connected";
    if (state & CB::kLocked)
        return "lock";
    return nullptr;
}

static QIcon cbStateIcon(uint32_t state)
{
    const QColor c = cbStateColor(state);
    if (!c.isValid())
        return {};
    static std::map<std::pair<QRgb, uint32_t>, QIcon> cache;
    const std::pair key{c.rgb(), state & (ChannelBox::kKeyed | ChannelBox::kKeyNow | ChannelBox::kMuted | ChannelBox::kBreakdown |
                                          ChannelBox::kDrivenKey | ChannelBox::kExpression | ChannelBox::kConstraint |
                                          ChannelBox::kConnected | ChannelBox::kLocked)};
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    constexpr qreal dpr = 2.0;
    QPixmap pm(QSize(kCbStateIconSize.width(), kCbStateIconSize.height()) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    // Keyed: a full bar on a key at this frame, a thinner one between keys.
    if (state & ChannelBox::kKeyed)
        p.fillRect(QRect(0, 1, (state & ChannelBox::kKeyNow) ? 4 : 2, 14), c);
    if (const char* glyph = cbStateGlyph(state))
    {
        QPixmap g = icon(glyph).pixmap(QSize(12, 12), dpr);
        QPainter gp(&g);
        gp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        gp.fillRect(g.rect(), c);
        gp.end();
        p.drawPixmap(QPointF(7, 2), g);
    }
    p.end();
    return cache[key] = QIcon(pm);
}

static QString cbStateText(uint32_t state, scene::NodeId source)
{
    using CB = ChannelBox;
    Graph& g = Graph::current();
    const scene::Node* src = g.node(source);
    const QString from = src ? QString::fromStdString(src->path()) : QString();
    QStringList parts;
    if (state & CB::kKeyed)
        parts << ((state & CB::kBreakdown) ? "Breakdown at this frame" : (state & CB::kKeyNow) ? "Key at this frame" : "Keyed") +
                     QString(" (%1); click for the Graph Editor").arg(from);
    if (state & CB::kMuted)
        parts << "Muted";
    if (state & CB::kDrivenKey)
        parts << "Driven key: " + from;
    if (state & CB::kExpression)
        parts << "Expression: " + from;
    if (state & CB::kConstraint)
        parts << "Constraint: " + from;
    if (state & CB::kConnected)
        parts << "Connected from " + from;
    if (!(state & CB::kKeyed) && src)
        parts.back() += "; click to select it";
    if (state & CB::kLocked)
        parts << "Locked";
    return parts.join("\n");
}

uint32_t ChannelBox::stateOf(scene::PlugRef ref, scene::NodeId* source)
{
    Graph& g = Graph::current();
    uint32_t s = 0;
    if (source)
        *source = scene::kNoNode;
    if (!ref.valid() || !g.node(ref.node))
        return 0;
    if (g.isLocked(ref))
        s |= kLocked;
    if (const scene::NodeId curve = scene::anim::curveFor(g, ref))
    {
        s |= kKeyed;
        if (source)
            *source = curve;
        const scene::Node& c = *g.node(curve);
        if (scene::anim::hasKeyAt(c, g.time()))
            s |= kKeyNow;
        if (const int mute = c.type().attrIndex("mute"); mute >= 0 && c.as<bool>(mute))
            s |= kMuted;
        auto breakdownNow = [&](const auto& keys) {
            for (const auto& k : keys)
                if (std::abs(k.time - g.time()) < 1e-6 && k.breakdown)
                    return true;
            return false;
        };
        if ((s & kKeyNow) && (scene::anim::isRotationCurve(c) ? breakdownNow(scene::anim::rotationCurveOf(c).keys)
                                                              : breakdownNow(scene::anim::curveOf(c).keys)))
            s |= kBreakdown;
        return s;
    }
    std::optional<scene::PlugRef> src = g.source(ref);
    if (!src && ref.component >= 0)
        src = g.source({ref.node, ref.attr, -1, ref.element});
    if (!src)
        return s;
    const scene::Node* from = g.node(src->node);
    if (source)
        *source = src->node;
    const std::string& type = from->type().registered().name;
    if (from->type().isA("drivenChannel"))
        s |= kDrivenKey;
    else if (from->type().attrIndex("lilaExpression") >= 0 || type.find("xpression") != std::string::npos)
        s |= kExpression;
    else if (type.find("onstraint") != std::string::npos)
        s |= kConstraint;
    else
        s |= kConnected;
    return s;
}

bool ChannelBox::passesFilter(const Row& row, scene::NodeId lead) const
{
    if (row.header)
        return true;
    Graph& g = Graph::current();
    const scene::NodeId owner = row.node ? row.node : lead;
    const scene::Node* n = g.node(owner);
    if (!n)
        return false;
    const scene::AttrDef& def = n->type().attrs[size_t(row.attrIndex)];
    if (const QString q = mFilter ? mFilter->text().trimmed() : QString(); !q.isEmpty())
    {
        const QString label = row.label.empty() ? attrView::label(def, row.component) : QString::fromStdString(row.label);
        const std::string& shortName = row.component >= 0 && def.type == scene::PlugType::Vec3 ? def.childShort[size_t(row.component)] : def.shortName;
        if (!label.contains(q, Qt::CaseInsensitive) && !QString::fromStdString(row.attr).contains(q, Qt::CaseInsensitive) &&
            !QString::fromStdString(shortName).contains(q, Qt::CaseInsensitive))
            return false;
    }
    const uint32_t s = stateOf({owner, row.attrIndex, row.component, row.element});
    bool own = false;
    for (const scene::AttrDef& d : n->type().registered().ownAttrs)
        own |= d.name == def.name;
    const std::map<std::string, bool> has = {
        {"Keyed", (s & kKeyed) != 0},
        {"Driven", (s & (kDrivenKey | kExpression | kConstraint)) != 0},
        {"Connected", (s & (kConnected | kDrivenKey | kExpression | kConstraint | kKeyed)) != 0},
        {"User", (def.flags & scene::AttrUser) != 0},
        {"Locked", (s & kLocked) != 0},
        {"TRS", def.name == "translate" || def.name == "rotate" || def.name == "scale"},
        {"Type", own},
    };
    for (const auto& [chip, state] : mChips)
        if (state != 0 && has.at(chip) != (state == 1))
            return false;
    return true;
}

void ChannelBox::refreshStrip()
{
    Graph& g = Graph::current();
    const auto& sel = g.selection();
    std::vector<scene::NodeId> shown;
    for (int i = 0; i < mStrip->count(); ++i)
        shown.push_back(scene::NodeId(mStrip->item(i)->data(Qt::UserRole).toULongLong()));
    const scene::NodeId lead = attrView::lead();
    if (shown != sel)
    {
        QSignalBlocker block(mStrip);
        mStrip->clear();
        for (scene::NodeId id : sel)
            if (const scene::Node* n = g.node(id))
            {
                auto* item = new QListWidgetItem(QString::fromStdString(attrView::showNamespaces() ? n->path() : n->name()), mStrip);
                item->setData(Qt::UserRole, qulonglong(id));
            }
    }
    for (int i = 0; i < mStrip->count(); ++i)
    {
        QListWidgetItem* item = mStrip->item(i);
        QFont f = item->font();
        f.setBold(scene::NodeId(item->data(Qt::UserRole).toULongLong()) == lead);
        item->setFont(f);
        item->setToolTip(!f.bold()                       ? "Click to make this the lead"
                         : attrView::channelBoxEditsAll() ? "The lead object: its values are shown"
                                                          : "The lead object: edits go here (Alt edits all)");
    }
    mStrip->setVisible(sel.size() > 1);
    if (!mStrip->isHidden())
    {
        static_cast<SelectionStrip*>(mStrip)->fitHeight();
        for (int i = 0; i < mStrip->count(); ++i)
            if (scene::NodeId(mStrip->item(i)->data(Qt::UserRole).toULongLong()) == lead)
                mStrip->scrollToItem(mStrip->item(i));
    }
}

void ChannelBox::noteEdit(const Row& row)
{
    if (attrView::multiEditHeld())
    {
        attrView::noteMultiEditUsed();
        return;
    }
    Graph& g = Graph::current();
    if (row.node || g.selection().size() < 2 || attrView::channelBoxEditsAll() || !attrView::hintEnabled())
        return;
    int holders = 0;
    for (const std::string& path : allTargets())
        holders += hasPlug(g, path + "." + row.attr) ? 1 : 0;
    const scene::Node* n = g.node(mNode);
    if (holders < 2 || !n)
        return;
    const QString text = QString("Changed %1 only. Hold Alt to change all %2.").arg(QString::fromStdString(n->name())).arg(holders);
    mHintText->setText(text);
    mHint->show();
    QTimer::singleShot(5000, mHint, [hint = QPointer<QWidget>(mHint), label = QPointer<QLabel>(mHintText), text] {
        if (hint && label && label->text() == text)
            hint->hide();
    });
}

void ChannelBox::dropPlug(const std::string& source, const std::string& target)
{
    if (source.empty() || source == target)
        return;
    try
    {
        if (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
        {
            attrView::noteMultiEditUsed();
            Graph& g = Graph::current();
            const double d = currentNumber(g, g.plug(source)), v = currentNumber(g, g.plug(target));
            ops::execute("driveChannel", {{"args", {target}}, {"driver", source}, {"keys", json::array({json::array({d, v}), json::array({d + 1.0, v + 1.0})})},
                                          {"force", true}},
                         CallSource::UI);
        }
        else
            ops::execute("connectAttr", {{"args", {source, target}}, {"force", true}}, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
}

void ChannelBox::showSettings(QWidget* anchor)
{
    auto* pop = new QFrame(window(), Qt::Popup);
    pop->setObjectName("cbSettingsPopover");
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setFrameShape(QFrame::StyledPanel);
    auto* f = new QFormLayout(pop);
    f->setContentsMargins(8, 8, 8, 8);
    auto* speed = new QDoubleSpinBox(pop);
    speed->setObjectName("cbScrubSpeed");
    speed->setRange(0.01, 100.0);
    speed->setDecimals(2);
    speed->setSingleStep(0.25);
    speed->setValue(attrView::scrubSpeed());
    auto* speedRow = new QHBoxLayout;
    speedRow->setSpacing(2);
    speedRow->addWidget(speed, 1);
    struct SpeedPreset
    {
        const char* name;
        const char* icon;
        double value;
        const char* tip;
    };
    // A quarter and four times sit between the Ctrl and Shift drag modifiers rather than repeating them.
    static const SpeedPreset kPresets[] = {
        {"Fine", "feather", 0.25, "Fine (0.25): small props, careful rotations"},
        {"Normal", "walk", 1.0, "Normal (1)"},
        {"Fast", "run", 4.0, "Fast (4): large sets and layouts"},
    };
    std::vector<QToolButton*> presetButtons;
    for (const SpeedPreset& p : kPresets)
    {
        auto* b = new QToolButton(pop);
        b->setObjectName(QString("cbScrubPreset_") + p.name);
        b->setIcon(icon(p.icon));
        b->setAutoRaise(true);
        b->setCheckable(true);
        b->setToolTip(p.tip);
        connect(b, &QToolButton::clicked, speed, [speed, v = p.value] { speed->setValue(v); });
        speedRow->addWidget(b);
        presetButtons.push_back(b);
    }
    const auto markPreset = [presetButtons](double v) {
        for (size_t i = 0; i < presetButtons.size(); ++i)
            presetButtons[i]->setChecked(std::abs(v - kPresets[i].value) < 1e-6);
    };
    markPreset(speed->value());
    connect(speed, &QDoubleSpinBox::valueChanged, pop, [markPreset](double v) {
        attrView::setScrubSpeed(v);
        markPreset(v);
    });
    f->addRow("Scrub speed", speedRow);
    auto* hyperCheck = new QCheckBox("Faster drags go further", pop);
    hyperCheck->setObjectName("cbHyperbolic");
    hyperCheck->setChecked(attrView::hyperbolicScrub());
    connect(hyperCheck, &QCheckBox::toggled, pop, [](bool on) { attrView::setHyperbolicScrub(on); });
    f->addRow("Hyperbolic", hyperCheck);
    auto* viewport = new QComboBox(pop);
    viewport->setObjectName("cbViewportScrub");
    viewport->addItem("Always", "always");
    viewport->addItem("Select tool only", "select");
    viewport->addItem("Off", "off");
    viewport->setCurrentIndex(std::max(0, viewport->findData(QString::fromStdString(attrView::viewportScrub()))));
    viewport->setToolTip("When a middle drag in a viewport scrubs the highlighted channels");
    connect(viewport, &QComboBox::activated, pop, [viewport](int i) { attrView::setViewportScrub(viewport->itemData(i).toString().toStdString()); });
    f->addRow("Viewport middle drag", viewport);
    auto* places = new QSpinBox(pop);
    places->setObjectName("cbPrecision");
    places->setRange(0, 10);
    places->setValue(attrView::precision());
    places->setToolTip("Decimal places, shared with the Attribute Editor");
    connect(places, &QSpinBox::valueChanged, pop, [](int v) { attrView::setPrecision(v); });
    f->addRow("Decimals", places);
    auto* ns = new QCheckBox("Show namespaces", pop);
    ns->setObjectName("cbNamespaces");
    ns->setChecked(attrView::showNamespaces());
    connect(ns, &QCheckBox::toggled, pop, [](bool on) { attrView::setShowNamespaces(on); });
    f->addRow(QString(), ns);
    auto* editAll = new QCheckBox("Edit all selected", pop);
    editAll->setObjectName("cbEditAll");
    editAll->setChecked(attrView::channelBoxEditsAll());
    editAll->setToolTip("Off: edits go to the lead object only, Alt edits all");
    connect(editAll, &QCheckBox::toggled, pop, [](bool on) { attrView::setChannelBoxEditsAll(on); });
    f->addRow(QString(), editAll);
    auto* hint = new QCheckBox("Hint after lead-only edits", pop);
    hint->setObjectName("cbHintPref");
    hint->setChecked(attrView::hintEnabled());
    connect(hint, &QCheckBox::toggled, pop, [](bool on) { attrView::setHintEnabled(on); });
    f->addRow(QString(), hint);
    pop->adjustSize();
    pop->move(anchor->mapToGlobal(QPoint(anchor->width() - pop->width(), anchor->height())));
    pop->show();
}

void ChannelBox::jumpToSource(int row)
{
    const Row& r = mRows[size_t(row)];
    const scene::NodeId owner = r.node ? r.node : mNode;
    scene::NodeId source = scene::kNoNode;
    const uint32_t s = stateOf({owner, r.attrIndex, r.component, r.element}, &source);
    if (s & kKeyed)
    {
        panels::show("graphEditor");
        return;
    }
    if (const scene::Node* src = Graph::current().node(source))
        try
        {
            ops::execute("select", {{"args", {src->path()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
}

std::vector<ChannelBox::Row> ChannelBox::buildRows(scene::NodeId lead) const
{
    std::vector<Row> rows;
    Graph& g = Graph::current();
    auto addChannels = [&](scene::Node* n, scene::NodeId owner) {
        const auto& attrs = n->type().attrs;
        for (int i = 0; i < int(attrs.size()); ++i)
        {
            const scene::AttrDef& a = attrs[i];
            const bool space = i == scene::Transform::kSpace && scene::spaces::hasSpaces(g, n->id());
            if (!(a.flags & (scene::AttrKeyable | scene::AttrChannelBox)) || ((a.flags & scene::AttrHidden) && !space))
                continue;
            if (a.flags & scene::AttrArray)
            {
                // One row per element of a number array; a vector array's elements stay in the attribute editor.
                if (a.type != scene::PlugType::Vec3)
                    for (const scene::PlugRef& e : g.elements({n->id(), i}))
                    {
                        std::string plug = g.plugName(e);
                        rows.push_back({plug.substr(plug.find('.') + 1), i, -1, a.type, owner, false, {}, e.element});
                    }
                continue;
            }
            if (a.type == scene::PlugType::Vec3)
                for (int c = 0; c < 3; ++c)
                    rows.push_back({a.childNames[c], i, c, scene::PlugType::Float, owner});
            else
                rows.push_back({a.name, i, -1, a.type, owner});
        }
    };
    scene::Node* n = g.node(lead);
    if (!n)
        return rows;
    addChannels(n, scene::kNoNode);
    if (!n->type().isA("mesh"))
        return rows;

    std::vector<scene::NodeId> inputs = scene::modifiers::stack(g, lead);
    std::reverse(inputs.begin(), inputs.end());
    // The node feeding the bottom of the stack (a generator such as polyCube) comes last, like Maya's INPUTS.
    scene::PlugRef bottom = inputs.empty() ? scene::PlugRef{lead, scene::MeshNode::kInMesh, -1}
                                           : scene::PlugRef{inputs.back(), 0, -1};
    if (auto src = g.source(bottom); src && g.node(src->node) && !g.node(src->node)->type().dag)
        inputs.push_back(src->node);
    for (scene::NodeId id : inputs)
    {
        scene::Node* in = g.node(id);
        if (!in)
            continue;
        rows.push_back({in->name(), -1, -1, scene::PlugType::Float, id, true});
        addChannels(in, id);
        if (in->type().isA("morph"))
            for (scene::NodeId t : scene::morphs::targetNodes(g, id))
                rows.push_back({"weight", scene::morphs::kWeight, -1, scene::PlugType::Float, t, false,
                                g.node(t)->as<std::string>(scene::morphs::kMorphName)});
    }
    return rows;
}

void ChannelBox::refresh()
{
    LILA_ZONE();

    // Don't yank a cell out from under the user's cursor.
    QWidget* focus = QApplication::focusWidget();
    if (focus && focus != mTable && mTable->isAncestorOf(focus))
        return;
    Graph& g = Graph::current();
    g.evaluate();
    scene::NodeId lead = attrView::lead();
    scene::Node* n = g.node(lead);
    refreshStrip();

    mUpdating = true;
    mNode = lead;
    mName->setText(n ? QString::fromStdString(n->name()) : QString());
    mName->setToolTip(n ? QString::fromStdString(n->fullPath()) : QString());
    mName->setEnabled(n != nullptr);

    std::vector<Row> rows;
    {
        // Filtered, keeping an INPUTS heading only when a row under it stays.
        std::vector<Row> all = buildRows(lead);
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (all[i].header)
            {
                size_t j = i + 1;
                while (j < all.size() && !all[j].header && !passesFilter(all[j], lead))
                    ++j;
                if (j < all.size() && !all[j].header)
                    rows.push_back(all[i]);
                continue;
            }
            if (passesFilter(all[i], lead))
                rows.push_back(all[i]);
        }
    }
    bool sameLayout = rows.size() == mRows.size() &&
                      std::equal(rows.begin(), rows.end(), mRows.begin(), [](const Row& a, const Row& b) {
                          return a.attr == b.attr && a.node == b.node && a.header == b.header && a.component == b.component && a.label == b.label &&
                                 a.element == b.element;
                      });
    // A scrub holds row indices, so it only ever sees values change.
    if (!sameLayout && mScrub)
    {
        mUpdating = false;
        return;
    }
    if (!sameLayout)
    {
        mRows = std::move(rows);
        mTable->clearSpans();
        mTable->setRowCount(int(mRows.size()));
        for (int r = 0; r < int(mRows.size()); ++r)
        {
            const Row& row = mRows[r];
            if (row.header)
            {
                auto* head = new QTableWidgetItem(QString::fromStdString(row.attr));
                head->setFlags(Qt::ItemIsEnabled);
                QFont hf = head->font();
                hf.setWeight(QFont::DemiBold);
                head->setFont(hf);
                head->setForeground(palette().color(QPalette::BrightText));
                if (scene::Node* in = g.node(row.node))
                    head->setToolTip(QString::fromStdString(in->type().name + (in->type().doc.empty() ? "" : "\n" + in->type().doc)));
                mTable->setItem(r, 0, head);
                mTable->setItem(r, 1, new QTableWidgetItem());
                mTable->item(r, 1)->setFlags(Qt::NoItemFlags);
                mTable->setSpan(r, 0, 1, 2);
                continue;
            }
            scene::Node* owner = g.node(row.node ? row.node : lead);
            const scene::AttrDef& def = owner->type().attrs[row.attrIndex];
            QString shown = row.label.empty() ? attrView::label(def, row.component) : QString::fromStdString(row.label);
            if (row.label.empty() && !def.niceName.empty() && attrView::nameMode() == attrView::NameMode::Nice)
            {
                const char* axes = (def.flags & scene::AttrColor) ? "RGB" : "XYZ";
                shown = QString::fromStdString(def.niceName) + (row.component >= 0 && row.component < 3 ? QString(" ") + axes[row.component] : QString());
            }
            auto* name = new QTableWidgetItem(shown);
            name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            name->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            name->setData(kTipBodyRole, QString::fromStdString(row.attr + (def.doc.empty() ? "" : "\n" + def.doc)));
            if (QString text = defaultText(def, row.component); !text.isEmpty())
                name->setData(kTipHintRole, resetHint(text));
            name->setToolTip(hintedTip(name->data(kTipBodyRole).toString(), name->data(kTipHintRole).toString()));
            mTable->setItem(r, 0, name);
            auto* value = new QTableWidgetItem();
            value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            mTable->setItem(r, 1, value);
        }
    }

    for (int r = 0; n && r < int(mRows.size()); ++r)
    {
        const Row& row = mRows[r];
        if (row.header)
            continue;
        scene::NodeId owner = row.node ? row.node : lead;
        scene::PlugRef ref{owner, row.attrIndex, row.component, row.element};
        const scene::AttrDef& def = g.node(owner)->type().attrs[row.attrIndex];
        scene::PlugValue v = g.value(scene::PlugRef{owner, row.attrIndex, -1, row.element});
        QString text;
        if (row.type == scene::PlugType::Bool)
            text = std::get<bool>(v) ? "on" : "off";
        else if (row.type == scene::PlugType::Enum)
        {
            int64_t idx = std::get<int64_t>(v);
            const auto& names = row.attrIndex == scene::Transform::kSpace && scene::spaces::hasSpaces(g, owner) ? scene::spaces::names(g, owner)
                                                                                                           : def.enumNames;
            text = idx >= 0 && idx < int64_t(names.size()) ? QString::fromStdString(names[size_t(idx)]) : QString::number(idx);
        }
        else
            text = attrView::formatNumber(numericOf(v, row.component));
        QTableWidgetItem* item = mTable->item(r, 1);
        item->setText(text);
        const bool numeric = row.type != scene::PlugType::Bool && row.type != scene::PlugType::Enum;
        item->setTextAlignment(numeric ? (Qt::AlignRight | Qt::AlignVCenter) : (Qt::AlignLeft | Qt::AlignVCenter));
        // Keyed channels get a gray tint, brighter on a key; other driven channels read as locked.
        scene::NodeId curve = scene::anim::curveFor(g, ref);
        bool driven = !curve && (g.source(ref).has_value() || g.source(scene::PlugRef{owner, row.attrIndex, -1, row.element}).has_value());
        QBrush tint;
        if (curve)
            tint = scene::anim::hasKeyAt(*g.node(curve), g.time()) ? themeShade(84) : themeShade(60);
        // Locked fields read as a flat light grey, Maya's blue-grey in grayscale.
        const bool locked = g.isLocked(ref);
        item->setBackground(locked ? QBrush(themeShade(112)) : tint);
        mTable->item(r, 0)->setBackground(tint);
        item->setForeground(locked ? themeShade(28) : driven ? themeShade(130) : palette().color(QPalette::Text));
        scene::NodeId source = scene::kNoNode;
        const uint32_t state = stateOf(ref, &source);
        QFont f = item->font();
        f.setItalic(driven);
        f.setStrikeOut((state & kMuted) != 0);
        item->setFont(f);
        item->setFlags(driven || locked ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                                        : (Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
        QTableWidgetItem* nameItem = mTable->item(r, 0);
        nameItem->setData(Qt::UserRole, int(state));
        nameItem->setIcon(cbStateIcon(state));
        QString tip = nameItem->data(kTipBodyRole).toString();
        if (const QString what = cbStateText(state, source); !what.isEmpty())
            tip = what + "\n" + tip;
        nameItem->setToolTip(hintedTip(tip, nameItem->data(kTipHintRole).toString()));
    }
    mUpdating = false;
}

void ChannelBox::commitCell(QTableWidgetItem* item)
{
    if (item->column() != 1 || item->row() >= int(mRows.size()))
        return;
    // Apply the same text to every selected channel row, like Maya.
    std::vector<int> rows;
    for (QTableWidgetItem* sel : mTable->selectedItems())
        if (std::find(rows.begin(), rows.end(), sel->row()) == rows.end())
            rows.push_back(sel->row());
    if (std::find(rows.begin(), rows.end(), item->row()) == rows.end())
        rows = {item->row()};
    QString text = item->text();
    {
        UndoChunk chunk("Set Channels");
        for (int r : rows)
            if (!mRows[r].header)
                apply(mRows[r], text);
    }
    noteEdit(mRows[item->row()]);
    scheduleRefresh();
}

void ChannelBox::toggleBool(int row)
{
    QTableWidgetItem* cell = mTable->item(row, 1);
    if (!cell || !(cell->flags() & Qt::ItemIsEditable))
        return;
    std::vector<int> rows;
    if (mTable->item(row, 0)->isSelected())
        for (int r : highlightedRows())
            if (mRows[r].type == scene::PlugType::Bool && mTable->item(r, 1) && (mTable->item(r, 1)->flags() & Qt::ItemIsEditable))
                rows.push_back(r);
    if (std::find(rows.begin(), rows.end(), row) == rows.end())
        rows.push_back(row);
    // Every channel takes the opposite of the clicked one, like Maya, rather than each flipping on its own.
    const QString text = cell->text() == "on" ? "off" : "on";
    {
        UndoChunk chunk("Toggle Channels");
        for (int r : rows)
            apply(mRows[r], text);
    }
    noteEdit(mRows[row]);
    scheduleRefresh();
}

void ChannelBox::apply(const Row& row, const QString& text)
{
    Graph& g = Graph::current();
    std::vector<std::string> names = row.node ? std::vector<std::string>{g.node(row.node)->path()} : targets();
    for (const std::string& name : names)
    {
        scene::Node* n = findNode(name);
        std::string plug = name + "." + row.attr;
        // Skip selected nodes that don't have this channel.
        try
        {
            if (!n)
                continue;
            g.plug(plug);
        }
        catch (const std::exception&)
        {
            continue;
        }
        try
        {
            std::optional<json> value = parseValue(g, plug, row.type, text);
            if (!value)
            {
                if (row.type == scene::PlugType::Enum)
                    continue;
                log::warn("channel box: can't read '{}'", text.toStdString());
                return;
            }
            // Changing space keeps the pose: the node is re-keyed in the new frame.
            if (row.attrIndex == scene::Transform::kSpace && scene::spaces::hasSpaces(g, n->id()))
                ops::execute("switchSpace", {{"args", {name}}, {"space", *value}}, CallSource::UI);
            else
                ops::execute("setAttr", {{"args", {plug, *value}}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
    }
}

bool ChannelBox::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == mTable && (e->type() == QEvent::KeyPress || e->type() == QEvent::ShortcutOverride))
    {
        auto* k = static_cast<QKeyEvent*>(e);
        bool copy = k->matches(QKeySequence::Copy), paste = k->matches(QKeySequence::Paste);
        if (!copy && !paste)
            return QWidget::eventFilter(obj, e);
        e->accept();
        if (e->type() == QEvent::KeyPress)
            copy ? copyValues() : pasteValues();
        return true;
    }
    if (obj != mTable->viewport())
        return QWidget::eventFilter(obj, e);
    // The menu opens on the press; Windows would otherwise send the context menu event on release.
    if (e->type() == QEvent::ContextMenu && static_cast<QContextMenuEvent*>(e)->reason() == QContextMenuEvent::Mouse)
        return true;
    auto* me = static_cast<QMouseEvent*>(e);
    switch (e->type())
    {
    case QEvent::MouseButtonDblClick:
        if (me->button() == Qt::LeftButton)
        {
            QModelIndex idx = mTable->indexAt(me->position().toPoint());
            if (idx.isValid() && idx.row() < int(mRows.size()) && !mRows[idx.row()].header && mRows[idx.row()].type == scene::PlugType::Bool)
            {
                toggleBool(idx.row());
                return true;
            }
        }
        break;
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::Drop:
    {
        auto* d = static_cast<QDropEvent*>(e);
        if (!d->mimeData()->hasFormat(kCbPlugMime))
            return false;
        const QModelIndex idx = mTable->indexAt(d->position().toPoint());
        const bool ok = idx.isValid() && idx.row() < int(mRows.size()) && !mRows[idx.row()].header;
        if (!ok)
        {
            d->ignore();
            return true;
        }
        d->acceptProposedAction();
        if (e->type() == QEvent::Drop)
        {
            const Row& row = mRows[size_t(idx.row())];
            scene::Node* owner = Graph::current().node(row.node ? row.node : mNode);
            if (owner)
                dropPlug(QString::fromUtf8(d->mimeData()->data(kCbPlugMime)).toStdString(), owner->path() + "." + row.attr);
        }
        return true;
    }
    case QEvent::MouseButtonPress:
        mDragRow = -1;
        if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier)
        {
            // A press on an already highlighted name can drag that channel onto another to connect it.
            const QModelIndex idx = mTable->indexAt(me->position().toPoint());
            if (idx.isValid() && idx.column() == 0 && idx.row() < int(mRows.size()) && !mRows[idx.row()].header && mTable->item(idx.row(), 0)->isSelected() &&
                me->position().x() >= 16)
            {
                mDragRow = idx.row();
                mDragFrom = me->position().toPoint();
            }
        }
        if (me->button() == Qt::LeftButton && me->position().x() < 16)
        {
            QModelIndex idx = mTable->indexAt(me->position().toPoint());
            if (idx.isValid() && idx.column() == 0 && idx.row() < int(mRows.size()) && !mRows[idx.row()].header &&
                (mTable->item(idx.row(), 0)->data(Qt::UserRole).toInt() & ~int(kLocked)))
            {
                jumpToSource(idx.row());
                return true;
            }
        }
        if (me->button() == Qt::RightButton && !mScrub)
        {
            Q_EMIT mTable->customContextMenuRequested(me->position().toPoint());
            return true;
        }
        if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier)
        {
            QModelIndex idx = mTable->indexAt(me->position().toPoint());
            if (idx.isValid() && idx.column() == 1 && idx.row() < int(mRows.size()) && !mRows[idx.row()].header &&
                mRows[idx.row()].type == scene::PlugType::Enum)
            {
                const int row = idx.row();
                QTimer::singleShot(0, this, [this, row] {
                    if (QTableWidgetItem* cell = mTable->item(row, 1); cell && (cell->flags() & Qt::ItemIsEditable))
                        mTable->editItem(cell);
                });
            }
        }
        // Left button is the view's own selection (drag a range, Ctrl toggle, Shift extend); middle drag is the virtual slider.
        if (me->button() == Qt::MiddleButton)
        {
            QModelIndex idx = mTable->indexAt(me->position().toPoint());
            std::vector<int> highlighted = highlightedRows();
            QPoint at = me->globalPosition().toPoint();
            mMidClick.press(at.x(), at.y(), mClock.elapsed());
            bool channel = idx.isValid() && idx.row() < int(mRows.size()) && !mRows[idx.row()].header;
            mMidRow = channel ? idx.row() : -1;
            if (idx.isValid() || !highlighted.empty())
            {
                // Pressing a channel outside the highlight makes it the one dragged; on a highlighted one, all of them drag.
                bool inHighlight = std::binary_search(highlighted.begin(), highlighted.end(), idx.row());
                if (idx.isValid() && (highlighted.empty() || (channel && !inHighlight)))
                    mTable->selectionModel()->setCurrentIndex(mTable->model()->index(idx.row(), 0),
                                                              QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                beginScrub(at);
                return true;
            }
        }
        break;
    case QEvent::MouseMove:
        if (mDragRow >= 0 && (me->buttons() & Qt::LeftButton) && (me->position().toPoint() - mDragFrom).manhattanLength() >= QApplication::startDragDistance())
        {
            const Row& row = mRows[size_t(mDragRow)];
            mDragRow = -1;
            if (scene::Node* owner = Graph::current().node(row.node ? row.node : mNode))
            {
                auto* mime = new QMimeData;
                mime->setData(kCbPlugMime, QByteArray::fromStdString(owner->path() + "." + row.attr));
                mime->setText(QString::fromStdString(owner->path() + "." + row.attr));
                auto* drag = new QDrag(mTable);
                drag->setMimeData(mime);
                drag->exec(Qt::LinkAction | Qt::CopyAction);
                return true;
            }
        }
        if (mMidClick.down())
            mMidClick.move(me->globalPosition().toPoint().x(), me->globalPosition().toPoint().y());
        if (mScrub)
        {
            scrub(me->globalPosition().toPoint(), me->modifiers());
            return !mScrub || mScrub->moved;
        }
        break;
    case QEvent::MouseButtonRelease:
        if (me->button() == Qt::MiddleButton && mMidClick.down())
        {
            QPoint at = me->globalPosition().toPoint();
            bool click = mMidClick.release(at.x(), at.y(), mClock.elapsed());
            bool moved = mScrub && mScrub->moved;
            if (mScrub)
                endScrub(moved);
            if (click && !moved && mMidRow >= 0 && mMidRow < int(mRows.size()))
                resetRows(resetTargets(mMidRow));
            mMidRow = -1;
            return true;
        }
        if (mScrub)
        {
            bool moved = mScrub->moved;
            endScrub(moved);
            return moved;
        }
        break;
    default: break;
    }
    return QWidget::eventFilter(obj, e);
}

void ChannelBox::beginScrub(QPoint pos)
{
    Scrub s;
    s.start = s.last = pos;
    for (QTableWidgetItem* item : mTable->selectedItems())
        if (item->column() == 0 && item->row() < int(mRows.size()))
        {
            const Row& r = mRows[item->row()];
            if (!r.header && (r.type == scene::PlugType::Float || r.type == scene::PlugType::Int))
                s.rows.push_back(item->row());
        }
    if (s.rows.empty())
        return;
    Graph& g = Graph::current();
    for (int r : s.rows)
        for (const std::string& name : mRows[r].node ? std::vector<std::string>{g.node(mRows[r].node)->path()} : targets())
        {
            try
            {
                scene::PlugRef p = g.plug(name + "." + mRows[r].attr);
                if (!g.isLocked(p))
                    s.saved.push_back({p, g.value(p)});
            }
            catch (const std::exception&)
            {
            }
        }
    mScrub = std::move(s);
}

void ChannelBox::scrub(QPoint pos, Qt::KeyboardModifiers mods)
{
    Scrub& s = *mScrub;
    // The release can go missing (focus stolen mid-drag); a move with no button held ends the drag.
    if (QApplication::mouseButtons() == Qt::NoButton)
    {
        endScrub(s.moved);
        return;
    }
    int dx = pos.x() - s.last.x();
    s.last = pos;
    if (!s.moved && std::abs(pos.x() - s.start.x()) < valueReset::kMaxTravel)
        return;
    if (!s.moved)
    {
        UndoStack::get().openChunk("Drag Channels");
        s.chunk = true;
        s.moved = true;
    }
    double step = attrView::scrubStep((mods & Qt::ControlModifier) ? 0.01 : (mods & Qt::ShiftModifier) ? 1.0 : 0.1, dx);
    s.accum += dx * step;
    // Stops where every channel has hit a limit, so dragging back moves them straight away.
    double lo = HUGE_VAL, hi = -HUGE_VAL;
    for (auto& [plug, value] : s.saved)
    {
        const scene::AttrDef& def = Graph::current().attr(plug);
        double base = numericOf(value, -1);
        lo = std::min(lo, def.minValue.value_or(-HUGE_VAL) - base);
        hi = std::max(hi, def.maxValue.value_or(HUGE_VAL) - base);
    }
    if (lo <= hi)
        s.accum = std::clamp(s.accum, lo, hi);
    if (mScrubTimer->isActive())
        return;
    qint64 since = mLastScrubApply.isValid() ? mLastScrubApply.elapsed() : 1000;
    mScrubTimer->start(int(std::max<qint64>(0, 16 - since)));
}

void ChannelBox::applyScrub()
{
    LILA_ZONE();

    if (!mScrub)
        return;
    mLastScrubApply.start();
    Scrub& s = *mScrub;
    Graph& g = Graph::current();
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    for (auto& [plug, value] : s.saved)
    {
        try
        {
            double v = clampToLimits(g.attr(plug), numericOf(value, -1) + s.accum);
            if (std::holds_alternative<int64_t>(value))
                g.setValue(plug, int64_t(std::llround(v)));
            else
                g.setValue(plug, v);
        }
        catch (const std::exception&)
        {
        }
    }
    u.setEnabled(was);
    refresh();
}

void ChannelBox::endScrub(bool commit)
{
    mScrubTimer->stop();
    Scrub s = std::move(*mScrub);
    mScrub.reset();
    if (!s.moved)
        return;
    Graph& g = Graph::current();
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    for (auto& [plug, value] : s.saved)
        g.setValue(plug, value);
    u.setEnabled(was);
    if (commit)
    {
        // Lands in the chunk opened when the drag started, so the drag undoes as one step.
        UndoChunk chunk("Drag Channels");
        for (auto& [plug, value] : s.saved)
        {
            double v = clampToLimits(g.attr(plug), numericOf(value, -1) + s.accum);
            json jv = std::holds_alternative<int64_t>(value) ? json(int64_t(std::llround(v))) : json(std::round(v * 1e4) / 1e4);
            try
            {
                ops::execute("setAttr", {{"args", {g.plugName(plug), jv}}}, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        }
    }
    if (s.chunk)
        UndoStack::get().closeChunk();
    scheduleRefresh();
}

std::vector<int> ChannelBox::resetTargets(int clicked) const
{
    std::vector<int> rows = highlightedRows();
    if (std::find(rows.begin(), rows.end(), clicked) == rows.end())
        rows = {clicked};
    return rows;
}

void ChannelBox::resetRows(const std::vector<int>& rows)
{
    Graph& g = Graph::current();
    std::vector<int> done;
    {
        UndoChunk chunk("Reset Channels");
        for (int r : rows)
        {
            if (r < 0 || r >= int(mRows.size()) || mRows[r].header)
                continue;
            const Row& row = mRows[r];
            bool any = false;
            for (const std::string& name : row.node ? std::vector<std::string>{g.node(row.node)->path()} : targets())
            {
                try
                {
                    std::string plug = name + "." + row.attr;
                    if (!hasPlug(g, plug))
                        continue;
                    scene::PlugRef ref = g.plug(plug);
                    std::optional<json> def = valueReset::plugDefault(g.attr(ref), ref.component);
                    if (!def)
                        continue;
                    ops::execute("setAttr", {{"args", {plug, *def}}}, CallSource::UI);
                    any = true;
                }
                catch (const std::exception& e)
                {
                    log::error("{}", e.what());
                }
            }
            if (any)
                done.push_back(r);
        }
    }
    for (int r : done)
        if (QTableWidgetItem* cell = mTable->item(r, 1))
            flashReset(mTable->viewport(), mTable->visualItemRect(cell));
    scheduleRefresh();
}

std::vector<int> ChannelBox::highlightedRows() const
{
    std::vector<int> rows;
    for (QTableWidgetItem* item : mTable->selectedItems())
        if (item->column() == 0 && item->row() < int(mRows.size()) && !mRows[item->row()].header)
            rows.push_back(item->row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

void ChannelBox::copyValues()
{
    Graph& g = Graph::current();
    if (!g.node(mNode))
        return;
    std::vector<CopiedValue> copied;
    QStringList lines;
    for (int r : highlightedRows())
    {
        const Row& row = mRows[r];
        scene::NodeId owner = row.node ? row.node : mNode;
        scene::Node* n = g.node(owner);
        if (!n)
            continue;
        scene::PlugValue v = g.value(scene::PlugRef{owner, row.attrIndex, -1, row.element});
        CopiedValue c{row.attr, row.node ? n->type().name : std::string(), json()};
        QString text;
        if (row.type == scene::PlugType::Bool)
        {
            c.value = std::get<bool>(v);
            text = std::get<bool>(v) ? "on" : "off";
        }
        else if (row.type == scene::PlugType::Enum || row.type == scene::PlugType::Int)
        {
            c.value = std::get<int64_t>(v);
            text = QString::number(std::get<int64_t>(v));
        }
        else
        {
            double d = numericOf(v, row.component);
            c.value = d;
            text = formatNumber(d);
        }
        lines << QString::fromStdString(row.attr) + " " + text;
        copied.push_back(std::move(c));
    }
    if (copied.empty())
        return;
    gCopied = std::move(copied);
    gCopiedText = lines.join('\n');
    QApplication::clipboard()->setText(gCopiedText);
}

void ChannelBox::pasteValues()
{
    Graph& g = Graph::current();
    QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty())
        return;

    // Text from elsewhere: "attr value" lines, or a single value (or expression) for every highlighted channel.
    std::vector<std::pair<std::string, QString>> lines;
    bool ours = text == gCopiedText.trimmed() && !gCopied.empty();
    if (!ours)
        for (const QString& line : text.split('\n', Qt::SkipEmptyParts))
        {
            QStringList parts = line.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (parts.size() != 2)
            {
                lines.clear();
                break;
            }
            lines.emplace_back(parts[0].toStdString(), parts[1]);
        }
    if (!ours && lines.empty())
    {
        UndoChunk chunk("Paste Channels");
        for (int r : highlightedRows())
            apply(mRows[r], text);
        scheduleRefresh();
        return;
    }

    auto set = [&](const std::string& plug, const json& value) {
        try
        {
            ops::execute("setAttr", {{"args", {plug, value}}}, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
    };
    auto parsed = [&](const std::string& plug, const QString& value) -> std::optional<json> {
        try
        {
            const scene::AttrDef& a = g.attr(g.plug(plug));
            scene::PlugRef ref = g.plug(plug);
            scene::PlugType type = ref.component >= 0 ? scene::PlugType::Float : a.type;
            return parseValue(g, plug, type, value);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    };

    UndoChunk chunk("Paste Channels");
    if (ours)
        for (const CopiedValue& c : gCopied)
        {
            if (c.inputType.empty())
            {
                for (const std::string& name : targets())
                    if (hasPlug(g, name + "." + c.attr))
                        set(name + "." + c.attr, c.value);
                continue;
            }
            // Input nodes are per object: paste onto the matching input shown here.
            for (const Row& row : mRows)
                if (row.node && !row.header && row.attr == c.attr && g.node(row.node) && g.node(row.node)->type().name == c.inputType)
                    set(g.node(row.node)->path() + "." + c.attr, c.value);
        }
    else
        for (auto& [attr, value] : lines)
        {
            bool any = false;
            for (const std::string& name : targets())
                if (std::string plug = name + "." + attr; hasPlug(g, plug))
                    if (auto v = parsed(plug, value))
                    {
                        set(plug, *v);
                        any = true;
                    }
            if (any)
                continue;
            for (const Row& row : mRows)
                if (row.node && !row.header && row.attr == attr && g.node(row.node))
                {
                    std::string plug = g.node(row.node)->path() + "." + attr;
                    if (auto v = parsed(plug, value))
                        set(plug, *v);
                }
        }
    scheduleRefresh();
}
} // namespace lila::ui
