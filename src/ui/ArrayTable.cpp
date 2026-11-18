#include "ui/ArrayTable.h"

#include "core/Profile.h"
#include "ui/Icons.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QRegularExpression>

#include <algorithm>
#include <set>

namespace lila::ui
{
using json = nlohmann::json;
using scene::Graph;
using scene::PlugType;

namespace
{
QString cellNumber(double v)
{
    QString s = QString::number(v, 'f', 4);
    if (s.contains('.'))
    {
        while (s.endsWith('0'))
            s.chop(1);
        if (s.endsWith('.'))
            s.chop(1);
    }
    return s == "-0" ? "0" : s;
}

std::vector<double> parseNumbers(const QString& text)
{
    std::vector<double> out;
    for (const QString& part : text.split(QRegularExpression("[,;\\s()]+"), Qt::SkipEmptyParts))
    {
        bool ok = false;
        const double v = part.toDouble(&ok);
        if (!ok)
            return {};
        out.push_back(v);
    }
    return out;
}
} // namespace

class ArrayTableModel final : public QAbstractTableModel
{
public:
    explicit ArrayTableModel(ArrayTable::Spec spec) : mSpec(std::move(spec)) {}

    const ArrayTable::Spec& spec() const { return mSpec; }
    void refresh()
    {
        if (rowCount() > 0 && columnCount() > 0)
            Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(mSpec.elements.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(mSpec.attrs.size()); }

    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (role == Qt::TextAlignmentRole)
            return int(o == Qt::Vertical ? Qt::AlignRight | Qt::AlignVCenter : Qt::AlignCenter);
        if (role != Qt::DisplayRole)
            return {};
        if (o == Qt::Vertical)
            return section < rowCount() ? QString::number(mSpec.elements[size_t(section)]) : QVariant();
        return section < mSpec.headers.size() ? mSpec.headers[section] : QVariant();
    }

    Qt::ItemFlags flags(const QModelIndex& i) const override
    {
        const scene::AttrDef* def = defOf(i);
        if (!def)
            return Qt::NoItemFlags;
        Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        if (!editable(i, *def))
            return f;
        return f | (def->type == PlugType::Bool ? Qt::ItemIsUserCheckable : Qt::ItemIsEditable);
    }

    QVariant data(const QModelIndex& i, int role) const override
    {
        const scene::AttrDef* def = defOf(i);
        if (!def)
            return {};
        if (role == Qt::TextAlignmentRole)
            return int((def->type == PlugType::String || def->type == PlugType::Enum ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
        if (role != Qt::DisplayRole && role != Qt::EditRole && role != Qt::CheckStateRole)
            return {};
        const scene::PlugValue v = Graph::current().value(ref(i));
        if (def->type == PlugType::Bool)
        {
            if (role != Qt::CheckStateRole)
                return {};
            const bool* b = std::get_if<bool>(&v);
            return b && *b ? Qt::Checked : Qt::Unchecked;
        }
        if (role == Qt::CheckStateRole)
            return {};
        return text(v, *def);
    }

    bool setData(const QModelIndex& i, const QVariant& value, int role) override
    {
        const scene::AttrDef* def = defOf(i);
        if (!def || !mSpec.set)
            return false;
        json v;
        if (def->type == PlugType::Bool && role == Qt::CheckStateRole)
            v = value.toInt() == Qt::Checked;
        else if (role != Qt::EditRole)
            return false;
        else if (std::optional<json> parsed = parse(value.toString(), *def))
            v = *parsed;
        else
            return false;
        const std::string plug = Graph::current().plugName(ref(i));
        mSpec.set(plug.substr(plug.find('.') + 1), v);
        Q_EMIT dataChanged(i, i);
        return true;
    }

private:
    scene::PlugRef ref(const QModelIndex& i) const
    {
        return {mSpec.node, mSpec.attrs[size_t(i.column())], -1, mSpec.elements[size_t(i.row())]};
    }

    const scene::AttrDef* defOf(const QModelIndex& i) const
    {
        const scene::Node* n = Graph::current().node(mSpec.node);
        if (!n || !i.isValid() || i.row() >= rowCount() || i.column() >= columnCount())
            return nullptr;
        return &n->type().attrs[size_t(mSpec.attrs[size_t(i.column())])];
    }

    bool editable(const QModelIndex& i, const scene::AttrDef& def) const
    {
        if (!(def.flags & scene::AttrInput) || Graph::current().source(ref(i)))
            return false;
        switch (def.type)
        {
        case PlugType::Bool:
        case PlugType::Int:
        case PlugType::Float:
        case PlugType::Vec3:
        case PlugType::String:
        case PlugType::Enum: return true;
        default: return false;
        }
    }

    static QString text(const scene::PlugValue& v, const scene::AttrDef& def)
    {
        if (const int64_t* n = std::get_if<int64_t>(&v))
        {
            if (def.type == PlugType::Enum && *n >= 0 && size_t(*n) < def.enumNames.size())
                return QString::fromStdString(def.enumNames[size_t(*n)]);
            return QString::number(*n);
        }
        if (const double* d = std::get_if<double>(&v))
            return cellNumber(*d);
        if (const glm::dvec3* p = std::get_if<glm::dvec3>(&v))
            return QString("%1, %2, %3").arg(cellNumber(p->x), cellNumber(p->y), cellNumber(p->z));
        if (const std::string* s = std::get_if<std::string>(&v))
            return QString::fromStdString(*s);
        if (const bool* b = std::get_if<bool>(&v))
            return *b ? "on" : "off";
        return QString::fromStdString(scene::toJson(v).dump());
    }

    static std::optional<json> parse(const QString& text, const scene::AttrDef& def)
    {
        const QString t = text.trimmed();
        switch (def.type)
        {
        case PlugType::String: return json(text.toStdString());
        case PlugType::Enum:
            for (size_t k = 0; k < def.enumNames.size(); ++k)
                if (t.compare(QString::fromStdString(def.enumNames[k]), Qt::CaseInsensitive) == 0)
                    return json(int64_t(k));
            [[fallthrough]];
        case PlugType::Int:
        {
            const std::vector<double> n = parseNumbers(t);
            return n.size() == 1 ? std::optional<json>(int64_t(std::llround(n[0]))) : std::nullopt;
        }
        case PlugType::Float:
        {
            const std::vector<double> n = parseNumbers(t);
            return n.size() == 1 ? std::optional<json>(n[0]) : std::nullopt;
        }
        case PlugType::Vec3:
        {
            const std::vector<double> n = parseNumbers(t);
            return n.size() == 3 ? std::optional<json>(json::array({n[0], n[1], n[2]})) : std::nullopt;
        }
        default: return std::nullopt;
        }
    }

    ArrayTable::Spec mSpec;
};

ArrayTable::ArrayTable(Spec spec, int visibleRows, QWidget* parent) : QTableView(parent)
{
    LILA_ZONE();
    setObjectName("attributeArrayTable");
    mModel = new ArrayTableModel(std::move(spec));
    mModel->setParent(this);
    setModel(mModel);
    verticalHeader()->setDefaultSectionSize(20);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    horizontalHeader()->setVisible(mModel->columnCount() > 1);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::AnyKeyPressed | QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setWordWrap(false);
    setCornerButtonEnabled(false);
    // Uniform rows let the view skip measuring 70k of them.
    verticalHeader()->setMinimumSectionSize(20);
    const int header = horizontalHeader()->isVisible() ? horizontalHeader()->sizeHint().height() : 0;
    setFixedHeight(header + 20 * std::min(visibleRows, std::max(1, mModel->rowCount())) + 2 * frameWidth());
}

void ArrayTable::refresh() { mModel->refresh(); }

std::vector<int> ArrayTable::selectedElements() const
{
    std::set<int> rows;
    for (const QModelIndex& i : selectionModel()->selectedIndexes())
        rows.insert(i.row());
    std::vector<int> out;
    for (int r : rows)
        out.push_back(mModel->spec().elements[size_t(r)]);
    return out;
}

void ArrayTable::copySelection() const
{
    const QModelIndexList sel = selectionModel()->selectedIndexes();
    if (sel.isEmpty())
        return;
    std::set<int> rows, cols;
    for (const QModelIndex& i : sel)
    {
        rows.insert(i.row());
        cols.insert(i.column());
    }
    QStringList lines;
    for (int r : rows)
    {
        QStringList cells;
        for (int c : cols)
        {
            const QModelIndex i = mModel->index(r, c);
            const QVariant check = mModel->data(i, Qt::CheckStateRole);
            cells << (check.isValid() ? (check.toInt() == Qt::Checked ? "on" : "off") : mModel->data(i, Qt::DisplayRole).toString());
        }
        lines << cells.join('\t');
    }
    QApplication::clipboard()->setText(lines.join('\n'));
}

void ArrayTable::removeSelection()
{
    const std::vector<int> elements = selectedElements();
    if (!elements.empty() && mModel->spec().remove)
        mModel->spec().remove(elements);
}

void ArrayTable::keyPressEvent(QKeyEvent* e)
{
    if (e->matches(QKeySequence::Copy))
        return copySelection();
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && state() != QAbstractItemView::EditingState)
        return removeSelection();
    QTableView::keyPressEvent(e);
}

void ArrayTable::contextMenuEvent(QContextMenuEvent* e)
{
    const std::vector<int> elements = selectedElements();
    if (elements.empty())
        return;
    QMenu menu(this);
    menu.addAction(icon("copy"), "Copy", QKeySequence::Copy, this, [this] { copySelection(); });
    if (mModel->spec().remove)
        menu.addAction(icon("trash"), elements.size() == 1 ? "Delete Element" : QString("Delete %L1 Elements").arg(elements.size()),
                       QKeySequence::Delete, this, [this] { removeSelection(); });
    menu.exec(e->globalPos());
}
} // namespace lila::ui
