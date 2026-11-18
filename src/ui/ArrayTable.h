#pragma once

#include "scene/Graph.h"

#include <nlohmann/json.hpp>

#include <QTableView>

#include <functional>
#include <string>
#include <vector>

namespace lila::ui
{
class ArrayTableModel;

/**
 * A long array attribute as a table: a row per element and a column per child (one column for a plain array). Only the
 * rows in view are drawn, so arrays of any length open instantly. Cells edit in place, Delete removes the selected
 * elements and Ctrl+C copies them tab-separated.
 */
class ArrayTable final : public QTableView
{
public:
    struct Spec
    {
        scene::NodeId node = scene::kNoNode;
        /** Attribute index of each column. */
        std::vector<int> attrs;
        QStringList headers;
        /** Element indices, in order; arrays can be sparse. */
        std::vector<int> elements;
        /** Sets a plug given relative to the node ("big[3]", "points[3].x"). */
        std::function<void(const std::string& plug, const nlohmann::json& value)> set;
        std::function<void(const std::vector<int>& elements)> remove;
    };

    ArrayTable(Spec spec, int visibleRows, QWidget* parent = nullptr);

    /** Redraws the visible cells from the graph. */
    void refresh();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    std::vector<int> selectedElements() const;
    void copySelection() const;
    void removeSelection();

    ArrayTableModel* mModel = nullptr;
};
} // namespace lila::ui
