#include "ui/Theme.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "render/Renderer.h"
#include "ui/DocsUi.h"
#include "ui/Telemetry.h"
#include "ui/ThemeModel.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QAbstractItemView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace lila::ui
{
namespace fs = std::filesystem;

namespace
{
/**
 * Lila's style over Fusion. Alt belongs to the hotkeys (Alt+letter bindings, Alt-drag camera), so a lone Alt never
 * focuses the menu bar. Selected and hovered rows in lists and trees get a rounded, inset highlight: rounded at the
 * row's two ends, square between its cells.
 */
class NoAltMenuStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override
    {
        if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option))
        {
            // The row's own fill keeps alternating colours only; the cells draw the selection.
            if (element == PE_PanelItemViewRow && (item->state & State_Selected))
            {
                if (item->features & QStyleOptionViewItem::Alternate)
                    painter->fillRect(item->rect, item->palette.brush(QPalette::AlternateBase));
                return;
            }
            if (element == PE_PanelItemViewItem && drawRowHighlight(*item, painter))
                return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* ret) const override
    {
        if (hint == SH_MenuBar_AltKeyNavigation)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, ret);
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override
    {
        if (const auto* item = section(type == CT_MenuItem, option))
            return {size.width(), item->fontMetrics.height() + 6};
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override
    {
        if (element == CE_MenuTearoff)
            return drawTearRow(painter, option->rect, option->palette, option->state & State_Selected, {});
        const auto* item = section(element == CE_MenuItem, option);
        if (!item)
            return QProxyStyle::drawControl(element, option, painter, widget);
        painter->save();
        painter->setFont(item->font);
        // In a menu that tears off, a section title is its own tear-off row (see TearOff.cpp).
        if (widget && widget->property("lilaTearOff").toBool())
            drawTearRow(painter, item->rect, item->palette, widget->property("lilaHotSection").toRect() == item->rect, item->text);
        else
        {
            // Maya's titled separator: the title dimmed, then a rule running on from it to the menu's edge.
            QRect r = item->rect.adjusted(8, 2, -4, 0);
            painter->setPen(item->palette.color(QPalette::Disabled, QPalette::Text));
            QRect text;
            painter->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, item->text, &text);
            painter->setPen(item->palette.color(QPalette::Midlight));
            if (text.right() + 6 < r.right())
                painter->drawLine(text.right() + 6, r.center().y() + 1, r.right(), r.center().y() + 1);
        }
        painter->restore();
    }

private:
    /** A rounded highlight for a selected or hovered cell; false leaves the cell to Fusion. */
    static bool drawRowHighlight(const QStyleOptionViewItem& item, QPainter* p)
    {
        const bool selected = item.state & State_Selected;
        const bool hover = (item.state & State_MouseOver) && (item.state & State_Enabled);
        if (!selected && !hover)
            return false;
        if (item.backgroundBrush.style() != Qt::NoBrush)
            p->fillRect(item.rect, item.backgroundBrush);
        QColor fill;
        if (selected)
            fill = item.palette.color(item.state & State_Active ? QPalette::Active : QPalette::Inactive, QPalette::Highlight);
        else
        {
            fill = themeColor("hover");
            fill.setAlphaF(0.25f);
        }
        using Pos = QStyleOptionViewItem::ViewItemPosition;
        const Pos pos = item.viewItemPosition;
        const bool first = pos == Pos::Beginning || pos == Pos::OnlyOne || pos == Pos::Invalid;
        const bool last = pos == Pos::End || pos == Pos::OnlyOne || pos == Pos::Invalid;
        constexpr qreal kRadius = 3, kInset = 2;
        // Inset at the ends only, and widened under the corners between cells, so a row's cells join into one bar.
        QRectF r = QRectF(item.rect).adjusted(first ? kInset : -kRadius, 1, last ? -kInset : kRadius, -1);
        p->save();
        p->setClipRect(item.rect);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawRoundedRect(r, kRadius, kRadius);
        p->restore();
        return true;
    }

    /** The menu's dashed tear-off row, with a section's title at its left when it has one. */
    static void drawTearRow(QPainter* p, const QRect& rect, const QPalette& pal, bool hot, const QString& title)
    {
        p->fillRect(rect, pal.brush(hot ? QPalette::Highlight : QPalette::Button));
        int left = rect.x() + 2;
        if (!title.isEmpty())
        {
            QRect text;
            p->setPen(hot ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Disabled, QPalette::Text));
            p->drawText(rect.adjusted(8, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, title, &text);
            left = text.right() + 6;
        }
        int y = rect.y() + rect.height() / 2;
        int right = rect.x() + rect.width() - 4;
        if (left >= right)
            return;
        p->setPen(QPen(pal.dark().color(), 1, Qt::DashLine));
        p->drawLine(left, y - 1, right, y - 1);
        p->setPen(QPen(pal.light().color(), 1, Qt::DashLine));
        p->drawLine(left, y, right, y);
    }

    static const QStyleOptionMenuItem* section(bool menuItem, const QStyleOption* option)
    {
        const auto* item = menuItem ? qstyleoption_cast<const QStyleOptionMenuItem*>(option) : nullptr;
        return item && item->menuItemType == QStyleOptionMenuItem::Separator && !item->text.isEmpty() ? item : nullptr;
    }
};

/** The colour icon as luminance, alpha kept. */
QIcon grayscaleIcon(const QString& path)
{
    QImage img = QImage(path).convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
    {
        auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x)
        {
            int g = qGray(row[x]);
            row[x] = qRgba(g, g, g, qAlpha(row[x]));
        }
    }
    return QIcon(QPixmap::fromImage(img));
}

QIcon appIcon(bool mono)
{
    const QString color = QString::fromStdWString((paths::resourcesDir() / "icons" / "lila_color.png").wstring());
    return mono ? grayscaleIcon(color) : QIcon(color);
}

/**
 * Gives every top-level window (dialogs, floating docks, tool windows) the dark native title bar, and the grey icon in
 * it. The taskbar keeps the colour icon (the window's big icon) unless the UI is monotone.
 */
class DarkTitleBars : public QObject
{
public:
    explicit DarkTitleBars(QObject* parent) : QObject(parent), mMono(viewportDisplay::monotone()) {}

    /** Follows the monotone pref: the app icon and every window's big icon go grey or back to colour. */
    void setMonotone(bool mono)
    {
        if (mono == mMono)
            return;
        mMono = mono;
        qApp->setWindowIcon(appIcon(mono));
#ifdef _WIN32
        for (auto& [dpi, icon] : mBigIcons)
            DestroyIcon(icon);
        mBigIcons.clear();
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->testAttribute(Qt::WA_WState_Created))
                applyIcons(HWND(w->winId()));
#endif
    }

#ifdef _WIN32
    ~DarkTitleBars() override
    {
        if (mSmallIcon)
            DestroyIcon(mSmallIcon);
        for (auto& [dpi, icon] : mBigIcons)
            DestroyIcon(icon);
    }
#endif

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
#ifdef _WIN32
        // Qt sends its own icons again when the window icon changes, so ours go back on after.
        if (e->type() == QEvent::Show || e->type() == QEvent::WinIdChange || e->type() == QEvent::WindowIconChange)
            if (auto* w = qobject_cast<QWidget*>(obj); w && w->isWindow() && w->testAttribute(Qt::WA_WState_Created))
            {
                BOOL dark = themes::current()->base != "light";
                HWND hwnd = HWND(w->winId());
                DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                applyIcons(hwnd);
            }
#else
        (void)e;
        (void)obj;
#endif
        return false;
    }

private:
    bool mMono = false;

#ifdef _WIN32
    void applyIcons(HWND hwnd)
    {
        if (HICON icon = smallIcon())
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(icon));
        if (HICON icon = bigIcon(GetDpiForWindow(hwnd)))
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(icon));
    }

    HICON smallIcon()
    {
        if (!mSmallIcon)
        {
            auto path = paths::resourcesDir() / "icons" / "lila_grey.ico";
            int size = GetSystemMetrics(SM_CXSMICON);
            mSmallIcon = HICON(LoadImageW(nullptr, path.wstring().c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
        }
        return mSmallIcon;
    }

    // Qt builds the big icon at the 96-dpi size, which the taskbar shows at half size on a scaled display.
    HICON bigIcon(UINT dpi)
    {
        HICON& icon = mBigIcons[dpi];
        if (!icon)
        {
            auto path = paths::resourcesDir() / "icons" / (mMono ? "lila_grey.ico" : "lila.ico");
            int size = GetSystemMetricsForDpi(SM_CXICON, dpi);
            icon = HICON(LoadImageW(nullptr, path.wstring().c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
        }
        return icon;
    }

    HICON mSmallIcon = nullptr;
    std::map<UINT, HICON> mBigIcons;
#endif
};

void loadBundledFonts()
{
    auto dir = paths::resourcesDir() / "fonts";
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().extension() == ".ttf")
            QFontDatabase::addApplicationFont(QString::fromStdWString(entry.path().wstring()));
}
} // namespace

QFont monoFont()
{
    PrefStore& p = PrefStore::get();
    QFont f(QString::fromStdString(p.value<std::string>("general", "/ui/monoFont", "JetBrains Mono")));
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(p.value<double>("general", "/ui/monoFontSize", 9.5));
    return f;
}

namespace
{
QColor qcolor(themes::Rgba c) { return QColor(c.r, c.g, c.b, c.a); }

/** Whether every neutral anchor is its own grey, so greys map to themselves and nothing needs rewriting. */
bool monotoneRamp(const themes::Theme& t)
{
    for (const themes::TokenInfo& info : themes::tokens())
        if (info.level >= 0 && t.color(info.id) != themes::Rgba{uint8_t(info.level), uint8_t(info.level), uint8_t(info.level), 255})
            return false;
    return true;
}

bool gFirstRun = false;
/** The resolved theme last pushed into the palette, to skip prefs changes that leave it alone. */
std::string gApplied;

/**
 * Glyphs the app sheet draws (check mark, combo arrow), written as SVG in the theme's colours. Style sheets only take
 * images from files, and Tabler's SVGs draw in currentColor, which a sheet image cannot set.
 */
QString sheetGlyph(const char* name, const std::string& color)
{
    static const std::map<std::string, std::string> paths = {
        {"check", R"(<path d="M5 12l5 5l10 -10"/>)"},
        {"chevron-down", R"(<path d="M6 9l6 6l6 -6"/>)"},
        {"chevron-right", R"(<path d="M9 6l6 6l-6 6"/>)"},
    };
    fs::path dir = paths::userConfigDir() / "cache" / "sheet";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string hex = color.substr(1);
    fs::path file = dir / (std::string(name) + "-" + hex + ".svg");
    if (!fs::exists(file, ec))
    {
        std::ofstream out(file, std::ios::binary);
        out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke=")" << color
            << R"(" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">)" << paths.at(name) << "</svg>";
    }
    return QString::fromStdString(file.generic_string());
}

/**
 * The control vocabulary, app-wide: borderless controls separated by a fill step, fields one step above the panel,
 * selection as the brightest fill, text tabs with an underline, slim scroll bars and 2 px chrome gaps between panels.
 */
QString appStyleSheet(const themes::Theme& t)
{
    auto hex = [&](const char* id) { return themes::formatColor(t.color(id)); };
    std::map<QString, QString> v = {
        {"chrome", QString::fromStdString(hex("chrome"))},
        {"panel", QString::fromStdString(hex("window"))},
        {"field", QString::fromStdString(hex("input"))},
        {"raised", QString::fromStdString(hex("button"))},
        {"hover", QString::fromStdString(hex("hover"))},
        {"pressed", QString::fromStdString(hex("pressed"))},
        {"sel", QString::fromStdString(hex("checked"))},
        {"selText", QString::fromStdString(hex("checkedText"))},
        {"rule", QString::fromStdString(hex("rule"))},
        {"dim", QString::fromStdString(hex("dimText"))},
        {"text", QString::fromStdString(hex("text"))},
        {"bright", QString::fromStdString(hex("brightText"))},
        {"disabled", QString::fromStdString(hex("disabled"))},
        {"focus", QString::fromStdString(hex("accent"))},
        {"tooltip", QString::fromStdString(hex("tooltip"))},
        {"tooltipText", QString::fromStdString(hex("tooltipText"))},
        {"scroll", QString::fromStdString(hex("scrollbar"))},
        {"menu", QString::fromStdString(hex("menu"))},
        {"checkImg", sheetGlyph("check", hex("window"))},
        {"comboImg", sheetGlyph("chevron-down", hex("dimText"))},
    };
    QString sheet = R"(
        QMainWindow { background: $chrome; }
        QMainWindow::separator { background: $chrome; width: 2px; height: 2px; }
        QMainWindow::separator:hover { background: $pressed; }
        QDockWidget::title { background: $panel; padding: 7px 8px 6px 12px; }
        QToolTip { background: $tooltip; color: $tooltipText; border: 1px solid $rule; padding: 4px 6px; }

        QMenuBar { background: $chrome; padding: 0 4px; }
        QMenuBar::item { background: transparent; padding: 5px 9px; border-radius: 4px; color: $text; }
        QMenuBar::item:selected { background: $hover; }
        QMenuBar::item:pressed { background: $pressed; }

        QStatusBar { background: $chrome; color: $dim; }
        QStatusBar::item { border: none; }
        QStatusBar QToolButton { padding: 1px 8px; color: $dim; }
        QStatusBar QLabel { color: $dim; }

        QToolButton { background: transparent; border: none; border-radius: 4px; padding: 3px; color: $text; }
        QToolButton:hover { background: $hover; }
        QToolButton:pressed { background: $pressed; }
        QToolButton:checked { background: $sel; color: $selText; }
        QToolButton:disabled { color: $disabled; }
        QToolButton[popupMode="1"] { padding-right: 18px; }
        QToolButton::menu-button { border: none; border-left: 1px solid $chrome; width: 16px; border-top-right-radius: 4px; border-bottom-right-radius: 4px; }
        QToolButton::menu-button:hover { background: $pressed; }

        QPushButton { background: $raised; border: none; border-radius: 4px; padding: 5px 12px; color: $text; }
        QPushButton:hover { background: $hover; }
        QPushButton:pressed { background: $pressed; }
        QPushButton:checked { background: $sel; color: $selText; }
        QPushButton:disabled { color: $disabled; }
        QPushButton:default { background: $sel; color: $selText; }
        QPushButton:flat { background: transparent; }
        QPushButton:flat:hover { background: $hover; }

        QLineEdit, QAbstractSpinBox { background: $field; border: 1px solid transparent; border-radius: 4px; padding: 3px 6px;
                                      color: $text; selection-background-color: $sel; selection-color: $selText; }
        QLineEdit:hover, QAbstractSpinBox:hover { background: $hover; }
        QLineEdit:focus, QAbstractSpinBox:focus { border-color: $focus; background: $field; }
        QLineEdit:disabled, QAbstractSpinBox:disabled { color: $disabled; }
        QLineEdit[readOnly="true"] { background: transparent; }
        QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 0; border: none; }
        QAbstractSpinBox::up-arrow, QAbstractSpinBox::down-arrow { width: 0; height: 0; }

        QComboBox { background: $field; border: 1px solid transparent; border-radius: 4px; padding: 3px 6px; color: $text; }
        QComboBox:hover { background: $hover; }
        QComboBox:focus { border-color: $focus; }
        QComboBox:on { background: $pressed; }
        QComboBox:disabled { color: $disabled; }
        QComboBox::drop-down { border: none; width: 18px; }
        QComboBox::down-arrow { image: url($comboImg); width: 12px; height: 12px; }
        QComboBox QAbstractItemView { background: $menu; border: 1px solid $rule; selection-background-color: $sel; selection-color: $selText; outline: 0; }

        QCheckBox, QRadioButton { spacing: 8px; color: $text; }
        QCheckBox:disabled, QRadioButton:disabled { color: $disabled; }
        QCheckBox::indicator { width: 14px; height: 14px; border-radius: 3px; background: $field; border: 1px solid $rule; }
        QCheckBox::indicator:hover { background: $hover; }
        QCheckBox::indicator:checked { background: $bright; border-color: $bright; image: url($checkImg); }
        QCheckBox::indicator:indeterminate { background: $dim; border-color: $dim; }
        QCheckBox::indicator:disabled { background: transparent; border-color: $rule; }
        QCheckBox::indicator:checked:disabled { background: $disabled; border-color: $disabled; }

        QTabBar::tab { background: transparent; border: none; border-bottom: 2px solid transparent; padding: 6px 10px 5px; color: $dim; }
        QTabBar::tab:hover { color: $text; }
        QTabBar::tab:selected { color: $bright; border-bottom-color: $bright; }
        QTabBar::tab:disabled { color: $disabled; }
        QTabWidget::pane { border: none; }
        QTabBar::close-button { subcontrol-position: right; }

        QAbstractItemView { border: none; outline: 0; }
        QScrollArea { border: none; }
        QHeaderView::section { background: $panel; border: none; border-bottom: 1px solid $rule; padding: 4px 6px; color: $dim; }
        QGroupBox { border: none; margin-top: 22px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 0; padding: 0; color: $bright; }

        QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
        QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
        QScrollBar::handle { background: $scroll; border-radius: 3px; margin: 2px; }
        QScrollBar::handle:hover { background: $pressed; }
        QScrollBar::handle:vertical { min-height: 24px; }
        QScrollBar::handle:horizontal { min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; }
        QScrollBar::add-page, QScrollBar::sub-page { background: none; }

        QWidget[role="section"] { border-top: 1px solid $rule; }
        QWidget[role="section"][plain="true"] { border-top: none; }
        QLabel[role="hint"], QLabel[role="dim"] { color: $dim; }
        QToolButton[role="sectionHeader"] { background: transparent; color: $text; font-weight: 600; text-align: left; padding: 5px 2px; }
        QToolButton[role="sectionHeader"]:checked { background: transparent; color: $bright; }
        QToolButton[role="sectionHeader"]:hover { background: transparent; color: $bright; }
        QToolButton[role="tile"] { background: transparent; padding: 4px 2px 3px; color: $text; }
        QToolButton[role="tile"]:hover { background: $hover; }
        QToolButton[role="tile"]:pressed { background: $pressed; }
        QToolButton[role="tile"]:checked { background: $sel; color: $selText; }
        QToolButton[role="row"] { background: transparent; border: 1px solid transparent; border-radius: 6px; }
        QToolButton[role="row"]:hover { background: $hover; }
        QToolButton[role="row"]:pressed { background: $pressed; }
        QToolButton[role="row"]:checked { background: $sel; border-color: $focus; }
        QToolButton[role="row"][popupMode="1"] { padding-right: 18px; }
        QToolButton[role="row"]::menu-button { border: none; width: 18px; }
        QToolButton[role="row"]::menu-indicator { subcontrol-position: right center; right: 6px; }
        QToolButton[role="push"] { background: $raised; padding: 4px 10px; text-align: left; }
        QToolButton[role="push"]:hover { background: $hover; }
        QToolButton[role="push"]:pressed { background: $pressed; }
        QToolButton[role="push"]:checked { background: $sel; color: $selText; }
        QToolButton[role="push"]:disabled { color: $disabled; }
        QToolButton[role="push"][popupMode="1"] { padding-right: 24px; }
        QToolButton[role="overflow"] { background: transparent; color: $dim; text-align: left; padding: 4px 10px; border-radius: 6px; }
        QToolButton[role="overflow"]:hover { background: $hover; color: $text; }
        QToolButton[role="overflow"]::menu-indicator { image: none; width: 0; }
        QWidget[role="segmented"] { background: $chrome; border-radius: 5px; }
        QToolButton[role="segment"] { background: transparent; color: $dim; padding: 3px 10px; border-radius: 3px; font-weight: 500; }
        QToolButton[role="segment"]:hover { background: $hover; color: $text; }
        QToolButton[role="segment"]:checked { background: $sel; color: $selText; }
        QToolButton[role="segment"]:disabled { color: $disabled; }
        QToolButton[role="segment"]:checked:disabled { background: $hover; color: $disabled; }
        QDoubleSpinBox[role="slider"], QDoubleSpinBox[role="slider"]:hover, QDoubleSpinBox[role="slider"]:focus { background: transparent; }
        QDoubleSpinBox[role="axis"] { padding-left: 20px; }

        QWidget#layoutTabs { background: $panel; border-radius: 6px; }
        QWidget#layoutTabs QToolButton { padding: 4px 12px; color: $dim; font-weight: 500; }
        QWidget#layoutTabs QToolButton:hover { color: $text; background: $hover; }
        QWidget#layoutTabs QToolButton:checked { color: $selText; background: $sel; }
        QWidget#layoutTabs QToolButton[layoutMore="true"] { padding: 4px 6px; }
        QWidget#layoutTabs QToolButton[layoutMore="true"]::menu-indicator { image: none; width: 0; }
        QToolButton#topSearch { background: $panel; color: $dim; padding: 4px 10px; text-align: left; }
        QToolButton#topSearch:hover { background: $hover; color: $text; }
        QToolButton#unrealSend { background: $raised; color: $bright; padding: 4px 26px 4px 10px; font-weight: 500; }
        QToolButton#unrealSend:hover { background: $hover; }
        QToolButton#menuPin { background: $sel; color: $selText; padding: 3px 8px; }
        QToolButton#topFolder { color: $dim; padding: 3px 8px; }
        QToolButton#topFolder:hover { color: $text; }
        QToolButton#topFolder::menu-indicator { image: none; width: 0; }

        QToolBar#sidebarStatus QToolButton:checked { background: $raised; }
        QTabBar#shelfTabs::tab { padding: 4px 10px 3px; margin-right: 2px; }
        QWidget#shelfPanel QToolButton { padding: 2px 6px; }
    )";
    // Longest names first, so $selText is not read as $sel.
    std::vector<std::pair<QString, QString>> ordered(v.begin(), v.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [key, value] : ordered)
        sheet.replace("$" + key, value);
    return sheet;
}

void applyColors(QApplication& app)
{
    std::shared_ptr<const themes::Theme> theme = themes::current();
    const themes::Theme& t = *theme;
    auto tok = [&](const char* id) { return qcolor(t.color(id)); };
    auto gray = [&](int v) { return qcolor(themes::shade(t, v)); };
    QPalette p;
    p.setColor(QPalette::Window, tok("window"));
    p.setColor(QPalette::WindowText, tok("text"));
    p.setColor(QPalette::Base, tok("base"));
    p.setColor(QPalette::AlternateBase, tok("altBase"));
    p.setColor(QPalette::ToolTipBase, tok("tooltip"));
    p.setColor(QPalette::ToolTipText, tok("tooltipText"));
    p.setColor(QPalette::PlaceholderText, tok("disabled"));
    p.setColor(QPalette::Text, tok("text"));
    p.setColor(QPalette::Button, tok("button"));
    p.setColor(QPalette::ButtonText, tok("text"));
    p.setColor(QPalette::BrightText, tok("brightText"));
    p.setColor(QPalette::Highlight, tok("highlight"));
    p.setColor(QPalette::HighlightedText, tok("highlightText"));
    p.setColor(QPalette::Link, tok("accent"));
    p.setColor(QPalette::LinkVisited, gray(190));
    p.setColor(QPalette::Light, gray(78));
    p.setColor(QPalette::Midlight, gray(66));
    p.setColor(QPalette::Mid, gray(40));
    p.setColor(QPalette::Dark, gray(30));
    p.setColor(QPalette::Shadow, tok("shadow"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, tok("disabled"));
    p.setColor(QPalette::Disabled, QPalette::Highlight, gray(70));
    app.setPalette(p);

    QPalette menu = p;
    menu.setColor(QPalette::Base, tok("menu"));
    app.setPalette(menu, "QMenu");
    QPalette input = p;
    input.setColor(QPalette::Base, tok("input"));
    app.setPalette(input, "QLineEdit");
    app.setPalette(input, "QAbstractSpinBox");
    QPalette scroll = p;
    scroll.setColor(QPalette::Button, tok("scrollbar"));
    app.setPalette(scroll, "QScrollBar");
    // Lists and trees sit on the panel; fields are the step up, so a list is never a box inside a box.
    QPalette list = p;
    list.setColor(QPalette::Base, tok("window"));
    list.setColor(QPalette::AlternateBase, tok("altBase"));
    app.setPalette(list, "QAbstractItemView");
    QPalette code = p;
    code.setColor(QPalette::Base, tok("menu"));
    app.setPalette(code, "QPlainTextEdit");
    app.setPalette(code, "QTextEdit");

    QString sheet = appStyleSheet(t);
    if (app.styleSheet() != sheet)
        app.setStyleSheet(sheet);
    gApplied = themes::toText(t, true);
}

/**
 * Keeps widget style sheets on the theme: the sheet a widget is given is kept as its template, and what it shows is
 * that template with the greys mapped. Re-run over every widget when the theme changes.
 */
class StyleSheetThemer : public QObject
{
public:
    using QObject::QObject;

    static void retheme(QWidget* w)
    {
        if (w->property("lilaRawColors").toBool())
            return;
        QVariant tmpl = w->property("lilaQssTemplate");
        if (!tmpl.isValid())
            return;
        QString themed = themedStyleSheet(tmpl.toString());
        if (themed != w->styleSheet())
        {
            w->setProperty("lilaQssApplied", themed);
            w->setStyleSheet(themed);
        }
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::StyleChange && e->type() != QEvent::Polish)
            return false;
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w || w->property("lilaRawColors").toBool())
            return false;
        QString sheet = w->styleSheet();
        if (sheet.isEmpty() || sheet == w->property("lilaQssApplied").toString())
            return false;
        // A sheet set by the widget's own code: remember it, then show it themed.
        w->setProperty("lilaQssTemplate", sheet);
        w->setProperty("lilaQssApplied", sheet);
        retheme(w);
        return false;
    }
};

void followTheme(QApplication& app)
{
    applyColors(app);
    for (QWidget* w : QApplication::allWidgets())
    {
        StyleSheetThemer::retheme(w);
        w->update();
#ifdef _WIN32
        if (w->isWindow() && w->testAttribute(Qt::WA_WState_Created))
        {
            BOOL dark = themes::current()->base != "light";
            DwmSetWindowAttribute(HWND(w->winId()), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
#endif
    }
}

class ThemeCard : public QFrame
{
public:
    ThemeCard(const themes::Theme& t, QWidget* parent) : QFrame(parent), mTheme(t)
    {
        setFixedSize(206, 178);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QString::fromStdString(t.description));
    }

    std::function<void()> picked;
    bool selected = false;

protected:
    void mousePressEvent(QMouseEvent*) override
    {
        if (picked)
            picked();
    }

    void paintEvent(QPaintEvent*) override
    {
        // Drawn in the card's own theme, whatever the current one is.
        auto c = [&](const char* id) { return qcolor(mTheme.color(id)); };
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        p.setPen(QPen(selected ? c("highlight") : c("border"), selected ? 3.0 : 1.0));
        p.setBrush(c("window"));
        p.drawRoundedRect(r, 6, 6);
        QRectF title(r.left() + 8, r.top() + 8, r.width() - 16, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(c("panel"));
        p.drawRoundedRect(title, 2, 2);
        QRectF list(r.left() + 8, title.bottom() + 4, 64, 54);
        p.setBrush(c("base"));
        p.drawRect(list);
        p.setBrush(c("highlight"));
        p.drawRect(QRectF(list.left(), list.top() + 18, list.width(), 12));
        for (int i = 0; i < 4; ++i)
        {
            p.setBrush(i == 1 ? c("highlightText") : c("text"));
            p.drawRect(QRectF(list.left() + 5, list.top() + 6 + i * 12 + 1, 30 + (i * 7) % 20, 2));
        }
        QRectF view(list.right() + 6, list.top(), r.right() - 8 - list.right() - 6, list.height());
        p.setBrush(c("viewportBackground"));
        p.drawRect(view);
        QPointF o = view.center() + QPointF(-8, 10);
        p.setPen(QPen(c("axisX"), 2));
        p.drawLine(o, o + QPointF(24, 0));
        p.setPen(QPen(c("axisY"), 2));
        p.drawLine(o, o + QPointF(0, -24));
        p.setPen(QPen(c("axisZ"), 2));
        p.drawLine(o, o + QPointF(-12, 10));
        p.setPen(QPen(c("wireSelected"), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(view.right() - 26, view.top() + 8, 16, 16));

        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(c("text"));
        QRectF text(r.left() + 8, list.bottom() + 6, r.width() - 16, r.bottom() - list.bottom() - 10);
        p.drawText(text, Qt::AlignLeft | Qt::AlignTop, QString::fromStdString(mTheme.name));
        f.setBold(false);
        f.setPointSizeF(f.pointSizeF() * 0.92);
        p.setFont(f);
        p.setPen(c("dimText"));
        p.drawText(text.adjusted(0, QFontMetrics(font()).height() + 2, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   QString::fromStdString(mTheme.description));
    }

private:
    themes::Theme mTheme;
};
} // namespace

QLabel* emptyStateLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName("emptyState");
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setContentsMargins(16, 24, 16, 24);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, themeColor("dimText"));
    label->setPalette(pal);
    return label;
}

namespace
{
/** Keeps an empty-state label over an item view's viewport, shown only while the model is empty. */
class EmptyHint : public QObject
{
public:
    EmptyHint(QAbstractItemView* view, const QString& text) : QObject(view), mView(view)
    {
        mLabel = emptyStateLabel(text, view->viewport());
        mLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        view->viewport()->installEventFilter(this);
        if (QAbstractItemModel* m = view->model())
        {
            connect(m, &QAbstractItemModel::rowsInserted, this, &EmptyHint::update);
            connect(m, &QAbstractItemModel::rowsRemoved, this, &EmptyHint::update);
            connect(m, &QAbstractItemModel::modelReset, this, &EmptyHint::update);
            connect(m, &QAbstractItemModel::layoutChanged, this, &EmptyHint::update);
        }
        update();
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() == QEvent::Resize && mLabel)
            mLabel->setGeometry(mView->viewport()->rect());
        return false;
    }

private:
    void update()
    {
        // The view's model outlives its viewport while the view is destroyed, and empties itself on the way out.
        if (!mLabel)
            return;
        QAbstractItemModel* m = mView->model();
        mLabel->setGeometry(mView->viewport()->rect());
        mLabel->setVisible(!m || m->rowCount() == 0);
    }

    QAbstractItemView* mView;
    QPointer<QLabel> mLabel;
};
} // namespace

void setEmptyHint(QAbstractItemView* view, const QString& text)
{
    new EmptyHint(view, text);
}

QColor themeColor(std::string_view token)
{
    return qcolor(themes::current()->color(token));
}

QColor themeShade(int level, int alpha)
{
    return qcolor(themes::shade(level, uint8_t(std::clamp(alpha, 0, 255))));
}

glm::vec4 themeSrgb(std::string_view token, const glm::vec4& legacy)
{
    std::shared_ptr<const themes::Theme> t = themes::current();
    if (themes::isDefault(*t, token))
        return legacy;
    themes::Rgba c = t->color(token);
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, legacy.a};
}

glm::vec4 themeLinear(std::string_view token, const glm::vec4& legacyLinear)
{
    std::shared_ptr<const themes::Theme> t = themes::current();
    if (themes::isDefault(*t, token))
        return legacyLinear;
    themes::Rgba c = t->color(token);
    return {srgbToLinear(c.r / 255.0f), srgbToLinear(c.g / 255.0f), srgbToLinear(c.b / 255.0f), legacyLinear.a};
}

QString themedStyleSheet(const QString& qss)
{
    std::shared_ptr<const themes::Theme> t = themes::current();
    if (monotoneRamp(*t))
        return qss;
    static const QRegularExpression hex(R"((?<=[:\s,])#([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})\b)");
    static const QRegularExpression rgb(R"(rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))");
    auto mapped = [&](int v) { return QString::fromStdString(themes::formatColor(themes::shade(*t, v))); };
    QString out;
    for (const QRegularExpression* re : {&hex, &rgb})
    {
        const QString& in = re == &hex ? qss : out;
        QString next;
        qsizetype last = 0;
        for (auto it = re->globalMatch(in); it.hasNext();)
        {
            QRegularExpressionMatch m = it.next();
            int base = re == &hex ? 16 : 10;
            int r = m.captured(1).toInt(nullptr, base), g = m.captured(2).toInt(nullptr, base), b = m.captured(3).toInt(nullptr, base);
            next += in.mid(last, m.capturedStart() - last);
            next += r == g && g == b ? mapped(r) : m.captured(0);
            last = m.capturedEnd();
        }
        next += in.mid(last);
        out = next;
    }
    return out;
}

bool themeChooserPending()
{
    return gFirstRun && !PrefStore::get().value<bool>("general", "/ui/themeChosen", false);
}

void setThemeChooserDone()
{
    PrefStore::get().set("general", "/ui/themeChosen", true);
}

QWidget* makeThemeChooser(QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel("Choose a look", box);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    title->setFont(f);
    layout->addWidget(title);
    auto* hint = new QLabel("You can change it any time in Preferences > Interface > Theme.", box);
    hint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(hint);
    auto* grid = new QGridLayout();
    grid->setSpacing(10);
    auto cards = std::make_shared<std::vector<ThemeCard*>>();
    for (const char* name : {"Colour", "Monotone", "Light", "Midnight", "Ember", "Paper"})
    {
        const themes::Theme* t = themes::builtIn(name);
        auto* card = new ThemeCard(*t, box);
        card->selected = themes::currentName() == name;
        card->picked = [cards, card, n = std::string(name)] {
            for (ThemeCard* c : *cards)
            {
                c->selected = c == card;
                c->update();
            }
            themes::setCurrent(n);
            setThemeChooserDone();
        };
        grid->addWidget(card, int(cards->size()) / 3, int(cards->size()) % 3);
        cards->push_back(card);
    }
    grid->setColumnStretch(3, 1);
    layout->addLayout(grid);

    namespace vd = viewportDisplay;
    layout->addSpacing(12);
    auto* qualityTitle = new QLabel("Viewport quality", box);
    qualityTitle->setFont(f);
    layout->addWidget(qualityTitle);
    auto* qualityHint = new QLabel("Only the starting point: each of these toggles from the viewport toolbar any time.", box);
    qualityHint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(qualityHint);
    const vd::Quality recommended = vd::recommendedQuality();
    const std::string gpu = render::Renderer::get().gpuInfo().name;
    vd::applyQuality(recommended);
    struct Option
    {
        vd::Quality quality;
        const char* name;
        const char* detail;
    };
    const Option options[] = {
        {vd::Quality::Best, "Best quality", "Accurate ambient occlusion, 8x antialiasing and shadows."},
        {vd::Quality::Balanced, "Balanced", "Faster ambient occlusion that also darkens reflections, temporal antialiasing (refines when the view stops) and shadows."},
        {vd::Quality::Fastest, "Fastest", "No ambient occlusion, antialiasing or shadows. For laptops and integrated graphics."},
    };
    auto* group = new QButtonGroup(box);
    for (const Option& o : options)
    {
        QString label = o.name;
        if (o.quality == recommended)
            label += gpu.empty() ? QString(" (recommended)") : QString(" (recommended for %1)").arg(QString::fromStdString(gpu));
        auto* radio = new QRadioButton(label, box);
        radio->setChecked(o.quality == recommended);
        group->addButton(radio);
        QObject::connect(radio, &QRadioButton::toggled, radio, [q = o.quality](bool on) {
            if (on)
                vd::applyQuality(q);
        });
        auto* detail = new QLabel(o.detail, box);
        detail->setForegroundRole(QPalette::PlaceholderText);
        detail->setContentsMargins(22, 0, 0, 4);
        layout->addWidget(radio);
        layout->addWidget(detail);
    }
    return box;
}

namespace
{
/** Stays until Continue: Esc and the close button do nothing. */
class SetupDialog : public QDialog
{
public:
    using QDialog::QDialog;

    void reject() override {}

protected:
    void closeEvent(QCloseEvent* e) override { e->ignore(); }
};

QWidget* makeTelemetryChoice(QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel("Help improve Lila", box);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    title->setFont(f);
    layout->addWidget(title);
    auto* hint = new QLabel("Lila is made by one developer. Anonymous statistics show which tools get used and where Lila crashes, so it can be fixed. "
                            "Never your files, file names or scene content. Change it any time in "
                            "Preferences > General > Telemetry.",
                            box);
    hint->setForegroundRole(QPalette::PlaceholderText);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    for (auto [label, key] : {std::pair{"Send usage statistics (version, GPU, session length, which commands are used)", kTelemetryUsagePref},
                              std::pair{"Send crash reports", kTelemetryCrashPref}})
    {
        auto* check = new QCheckBox(label, box);
        check->setChecked(PrefStore::get().value<bool>("general", key, false));
        QObject::connect(check, &QCheckBox::toggled, check, [key](bool on) { PrefStore::get().set("general", key, on); });
        layout->addWidget(check);
    }

    auto* about = new QLabel("Optional, sent with usage statistics:", box);
    about->setContentsMargins(0, 6, 0, 0);
    layout->addWidget(about);
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    for (auto [label, key, choices] : {std::tuple{"Your role", telemetry::kRolePref, &telemetry::roleChoices()},
                                       std::tuple{"Team size", telemetry::kTeamSizePref, &telemetry::teamSizeChoices()},
                                       std::tuple{"Used before Lila", telemetry::kPreviousDccPref, &telemetry::previousDccChoices()}})
    {
        auto* combo = new QComboBox(box);
        const std::string current = PrefStore::get().value<std::string>("general", key, "");
        for (const auto& [shown, value] : *choices)
        {
            combo->addItem(QString::fromStdString(shown), QString::fromStdString(value));
            if (value == current)
                combo->setCurrentIndex(combo->count() - 1);
        }
        QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo, key](int) {
            PrefStore::get().set("general", key, combo->currentData().toString().toStdString());
        });
        form->addRow(label, combo);
    }
    layout->addLayout(form);
    auto* sent = new QPushButton("See Exactly What's Sent...", box);
    QObject::connect(sent, &QPushButton::clicked, sent, [box] { telemetry::showPending(box->window()); });
    auto* details = new QPushButton("Privacy Details...", box);
    QObject::connect(details, &QPushButton::clicked, details, [] { docs::open("privacy", docs::Route::Browser); });
    auto* sentRow = new QHBoxLayout;
    sentRow->addStretch(1);
    sentRow->addWidget(sent);
    sentRow->addWidget(details);
    sentRow->addStretch(1);
    layout->addLayout(sentRow);
    return box;
}
} // namespace

void runFirstRunSetup(QWidget* parent)
{
    // Usage statistics start ticked here, where they are seen; everywhere else an unset pref means off.
    if (PrefStore::get().value("general", kTelemetryUsagePref).is_null())
        PrefStore::get().set("general", kTelemetryUsagePref, true);
    SetupDialog dlg(parent);
    dlg.setWindowTitle("Welcome to Lila");
    dlg.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    dlg.setWindowModality(Qt::ApplicationModal);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(20, 16, 20, 14);
    layout->addWidget(makeThemeChooser(&dlg));
    layout->addSpacing(12);
    layout->addWidget(makeTelemetryChoice(&dlg));
    layout->addSpacing(8);
    auto* buttons = new QDialogButtonBox(&dlg);
    QPushButton* go = buttons->addButton("Continue", QDialogButtonBox::AcceptRole);
    go->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    layout->addWidget(buttons);
    dlg.exec();
    setThemeChooserDone();
}

void showTelemetrySettings(QWidget* parent)
{
    static QPointer<QDialog> open;
    if (!open)
    {
        open = new QDialog(parent);
        open->setAttribute(Qt::WA_DeleteOnClose);
        open->setWindowTitle("Telemetry and Privacy");
        auto* layout = new QVBoxLayout(open);
        layout->setContentsMargins(20, 16, 20, 14);
        layout->addWidget(makeTelemetryChoice(open));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, open);
        QObject::connect(buttons, &QDialogButtonBox::rejected, open, &QDialog::close);
        layout->addWidget(buttons);
    }
    open->show();
    open->raise();
    open->activateWindow();
}

void applyTheme(QApplication& app)
{
    app.setStyle(new NoAltMenuStyle(QStyleFactory::create("Fusion")));

    PrefStore& prefs = PrefStore::get();
    std::error_code ec;
    gFirstRun = !std::filesystem::exists(prefs.userDir() / "general.json", ec) && prefs.value("general", themes::kThemePref).is_null() &&
                QGuiApplication::platformName() != "offscreen";

    loadBundledFonts();
    app.setWindowIcon(appIcon(viewportDisplay::monotone()));
    QFont ui(QString::fromStdString(prefs.value<std::string>("general", "/ui/font", "Inter")));
    ui.setPointSizeF(prefs.value<double>("general", "/ui/fontSize", 9.0));
    ui.setHintingPreference(QFont::PreferNoHinting);
    // Tabular figures, so numbers in fields don't shift width as they change.
    ui.setFeature(QFont::Tag("tnum"), 1);
    app.setFont(ui);

    applyColors(app);
    app.installEventFilter(new StyleSheetThemer(&app));

    auto* titleBars = new DarkTitleBars(&app);
    app.installEventFilter(titleBars);
    QPointer<DarkTitleBars> bars(titleBars);
    PrefStore::get().onChanged([bars](const std::string& file) {
        if (file != "general")
            return;
        // Deferred: the theme cache drops on this same change, and every listener must have run first.
        QTimer::singleShot(0, qApp, [bars] {
            if (!bars)
                return;
            bars->setMonotone(viewportDisplay::monotone());
            if (themes::toText(*themes::current(), true) != gApplied)
                followTheme(*qApp);
        });
    });
}
} // namespace lila::ui
