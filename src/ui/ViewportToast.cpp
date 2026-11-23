#include "ui/ViewportToast.h"

#include "ops/Command.h"
#include "ui/Overlay.h"
#include "ui/Theme.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace lila::ui::viewportToast
{
namespace
{
constexpr double kFadeMs = 400.0;
constexpr double kMargin = 12.0;
constexpr double kHeight = 26.0;
constexpr double kGap = 6.0;

struct Toast
{
    QString text;
    QString key;
    QPointer<QWidget> view;
    bool everyView = false;
    qint64 expires = 0;
    Kind kind = Kind::Info;
};

std::function<QWidget*()>& activeProvider()
{
    static std::function<QWidget*()> f;
    return f;
}

std::vector<Toast>& toasts()
{
    static std::vector<Toast> t;
    return t;
}

QElapsedTimer& clock()
{
    static QElapsedTimer c = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return c;
}

/** Redraws the viewports while a toast is up so it fades, then once more to clear the last one. */
QTimer& ticker()
{
    static QTimer* t = [] {
        auto* timer = new QTimer();
        timer->setInterval(33);
        QObject::connect(timer, &QTimer::timeout, [timer] {
            const qint64 now = clock().elapsed();
            std::erase_if(toasts(), [now](const Toast& t) { return t.expires <= now || (!t.everyView && !t.view); });
            if (toasts().empty())
                timer->stop();
            viewportTools::requestRedraw();
        });
        return timer;
    }();
    return *t;
}
} // namespace

void show(const QString& text, QWidget* view, int durationMs, Kind kind, const QString& key)
{
    const qint64 expires = clock().elapsed() + std::max(durationMs, 1);
    const QString k = key.isEmpty() ? text : key;
    std::erase_if(toasts(), [&](const Toast& t) { return t.key == k && t.view == view && t.everyView == !view; });
    toasts().push_back({text, k, view, !view, expires, kind});
    if (!ticker().isActive())
        ticker().start();
    viewportTools::requestRedraw();
}

void showActive(const QString& text, int durationMs, Kind kind, const QString& key)
{
    show(text, activeProvider() ? activeProvider()() : nullptr, durationMs, kind, key);
}

void setActive(std::function<QWidget*()> active)
{
    activeProvider() = std::move(active);
}

void registerCommands()
{
    if (ops::find("viewportToast"))
        return;
    ops::registerCommand({
        .name = "viewportToast",
        .doc = "Pop up a short message in the bottom-right corner of the active viewport; it fades out after duration. "
               "Showing the same text again restarts it instead of adding another.",
        .positionalDoc = "text",
        .flags = {{.name = "all", .shortName = "a", .type = ArgType::Bool, .doc = "Show it in every viewport, not only the active one."},
                  {.name = "duration",
                   .shortName = "d",
                   .type = ArgType::Float,
                   .defaultValue = 2.0,
                   .doc = "Seconds before it is gone, including the fade.",
                   .min = 0.1,
                   .max = 60.0},
                  {.name = "error", .shortName = "e", .type = ArgType::Bool, .doc = "Mark it as a failure, with a red accent."},
                  {.name = "warning", .shortName = "w", .type = ArgType::Bool, .doc = "Mark it as a warning, with a warning accent and triangle."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string() || a.positional[0].get<std::string>().empty())
                throw CommandError("viewportToast: text expected");
            const QString text = QString::fromStdString(a.positional[0].get<std::string>());
            const int ms = int(a.get<double>("duration") * 1000.0);
            const Kind kind = a.get<bool>("error") ? Kind::Error : a.get<bool>("warning") ? Kind::Warning : Kind::Info;
            if (a.get<bool>("all"))
                show(text, nullptr, ms, kind);
            else
                showActive(text, ms, kind);
            return nullptr;
        },
        .examples = {"viewportToast(\"Exported 12 meshes\")", "viewportToast(\"Render done\", all=True, duration=4)"},
    });
}

void draw(OverlayPainter& p, const ViewInfo& info, const QWidget* view, const std::vector<QRectF>& occupied)
{
    const qint64 now = clock().elapsed();
    double bottom = info.height - kMargin;
    for (auto it = toasts().rbegin(); it != toasts().rend(); ++it)
    {
        if (it->expires <= now || (!it->everyView && it->view != view))
            continue;
        const float alpha = float(std::clamp((it->expires - now) / kFadeMs, 0.0, 1.0));
        const bool warning = it->kind == Kind::Warning;
        const QString& text = it->text;
        const double w = OverlayPainter::textWidth(text) + 20.0;
        QRectF r(info.width - kMargin - w, bottom - kHeight, w, kHeight);
        for (bool moved = true; moved;)
        {
            moved = false;
            for (const QRectF& o : occupied)
                if (o.intersects(r))
                {
                    r.moveBottom(o.top() - kGap);
                    moved = true;
                }
        }
        p.rect(r, {0.08f, 0.08f, 0.08f, 0.82f * alpha});
        glm::vec4 accent(0.35f, 0.65f, 1.0f, alpha);
        if (it->kind == Kind::Error)
            accent = {1.0f, 0.35f, 0.3f, alpha};
        else if (warning)
        {
            const QColor c = themeColor("issueWarning");
            accent = {float(c.redF()), float(c.greenF()), float(c.blueF()), alpha};
        }
        p.rect(QRectF(r.left(), r.top(), warning ? 4.0 : 3.0, r.height()), accent);
        p.text(QPointF(r.left() + 11.0, r.top() + 17.5), text, warning ? glm::vec4(accent.r, accent.g, accent.b, alpha) : glm::vec4(0.95f, 0.95f, 0.95f, alpha));
        bottom = r.top() - kGap;
    }
}
} // namespace lila::ui::viewportToast
