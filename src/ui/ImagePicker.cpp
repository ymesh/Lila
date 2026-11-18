#include "ui/ImagePicker.h"

#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "render/HdrImage.h"
#include "scene/Node.h"
#include "ui/Environments.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/TexturePicker.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace fs = std::filesystem;

namespace lila::ui
{
namespace
{
const QString kImageFiles = "Images (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.bmp *.tif *.tiff *.psd *.webp)";

} // namespace

namespace imagePicker
{
namespace
{

std::map<std::string, std::unique_ptr<Library>, std::less<>>& libraries()
{
    static std::map<std::string, std::unique_ptr<Library>, std::less<>> m;
    return m;
}

/** "type.attr" to library id. */
std::map<std::string, std::string, std::less<>>& bindings()
{
    static std::map<std::string, std::string, std::less<>> m;
    return m;
}

} // namespace

bool isImageFile(const fs::path& p)
{
    std::string ext = paths::toUtf8(p.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const std::vector<std::string> kExts{".png", ".jpg", ".jpeg", ".tga", ".hdr", ".exr", ".bmp", ".tif", ".tiff", ".psd", ".webp"};
    return std::ranges::find(kExts, ext) != kExts.end();
}

namespace
{

/** The images in dir, by name, under group. */
std::vector<Entry> folderEntries(const fs::path& dir, const QString& group, bool storeFileName)
{
    std::vector<fs::path> files;
    std::error_code ec;
    if (!dir.empty())
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && isImageFile(e.path()))
                files.push_back(e.path());
    std::ranges::sort(files);
    std::vector<Entry> out;
    for (const fs::path& f : files)
        out.push_back({storeFileName ? paths::toUtf8(f.filename()) : paths::storedPath(f), QString::fromStdString(environments::displayName(f)), f, group});
    return out;
}

fs::path projectSubdir(const char* name) { return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / name : fs::path(); }

void addBuiltIns()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    const fs::path user = PrefStore::get().userDir() / "hdri";
    libraries()["hdri"] = std::make_unique<Library>(Library{
        .id = "hdri",
        .title = "HDRIs",
        .entries =
            [user] {
                std::vector<Entry> out = folderEntries(paths::resourcesDir() / "hdri", "Lila", true);
                for (Entry& e : folderEntries(user, "Mine", false))
                    out.push_back(std::move(e));
                for (Entry& e : folderEntries(projectSubdir("hdri"), "Project", false))
                    out.push_back(std::move(e));
                return out;
            },
        .userDir = user,
        .projectDir = [] { return projectSubdir("hdri"); },
        .filter = "HDR images (*.hdr *.exr);;" + kImageFiles,
        .extensions = {".hdr", ".exr"},
    });
    bindings()["light.texture"] = "hdri";
}

/** Linear float RGB to an sRGB 8-bit thumbnail, tone mapped when bright. */
QImage toThumb(int w, int h, const std::function<glm::vec3(int, int)>& at, int size)
{
    const double scale = double(size) / std::max(w, h);
    const int tw = std::max(1, int(std::lround(w * scale))), th = std::max(1, int(std::lround(h * scale)));
    QImage img(tw, th, QImage::Format_RGB32);
    auto enc = [](float c) {
        c = c / (1.0f + c * 0.25f);
        c = std::clamp(c, 0.0f, 1.0f);
        return int(std::lround(255.0f * (c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f)));
    };
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x)
        {
            const glm::vec3 c = at(std::min(w - 1, int((x + 0.5) * w / tw)), std::min(h - 1, int((y + 0.5) * h / th)));
            img.setPixel(x, y, qRgb(enc(c.r), enc(c.g), enc(c.b)));
        }
    return img;
}

QImage loadThumb(const fs::path& file, int size)
{
    std::string ext = paths::toUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".hdr")
    {
        render::HdrImage hdr;
        std::string error;
        if (!render::loadHdr(file, hdr, error) || hdr.width <= 0)
            return {};
        return toThumb(hdr.width, hdr.height, [&](int x, int y) { return hdr.at(x, y); }, size);
    }
    if (ext == ".exr" || ext == ".tif" || ext == ".tiff" || ext == ".psd" || ext == ".tga" || ext == ".webp")
    {
        const image::Image im = image::read(file);
        if (im.width <= 0)
            return {};
        // Only EXR holds linear light; the rest are display encoded already, so undo it for toThumb.
        const bool linear = ext == ".exr";
        return toThumb(im.width, im.height,
                       [&](int x, int y) {
                           const float* p = &im.rgba[(size_t(y) * size_t(im.width) + size_t(x)) * 4];
                           auto lin = [&](float c) { return linear ? c : std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); };
                           return glm::vec3(lin(p[0]), lin(p[1]), lin(p[2]));
                       },
                       size);
    }
    QImage img(QString::fromStdString(paths::toUtf8(file)));
    return img.isNull() ? img : img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
} // namespace

void addLibrary(Library lib)
{
    addBuiltIns();
    auto& slot = libraries()[lib.id];
    if (slot)
        *slot = std::move(lib);
    else
        slot = std::make_unique<Library>(std::move(lib));
}

const Library* library(std::string_view id)
{
    addBuiltIns();
    auto it = libraries().find(id);
    return it == libraries().end() ? nullptr : it->second.get();
}

void setLibraryFor(std::string nodeType, std::string attr, std::string lib)
{
    addBuiltIns();
    bindings()[nodeType + "." + attr] = std::move(lib);
}

const Library* libraryFor(const scene::NodeType& type, std::string_view attr)
{
    addBuiltIns();
    for (const scene::NodeType* t = &type; t; t = t->base)
        if (auto it = bindings().find(t->name + "." + std::string(attr)); it != bindings().end())
            return library(it->second);
    return nullptr;
}

QIcon thumbnail(const fs::path& file, int size)
{
    static std::map<std::pair<std::string, int>, std::pair<fs::file_time_type, QIcon>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec)
        return {};
    const auto key = std::pair(paths::toUtf8(file), size);
    if (auto it = cache.find(key); it != cache.end() && it->second.first == stamp)
        return it->second.second;
    QIcon icon;
    try
    {
        if (QImage img = loadThumb(file, size); !img.isNull())
            icon = QIcon(QPixmap::fromImage(img));
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, std::string("thumbnail ") + key.first + ": " + e.what());
    }
    cache[key] = {stamp, icon};
    return icon;
}
QImage thumbnailImage(const fs::path& file, int size)
{
    try
    {
        return loadThumb(file, size);
    }
    catch (const std::exception&)
    {
        return {};
    }
}
} // namespace imagePicker

ImagePicker::ImagePicker(const imagePicker::Library* library, QString title, QWidget* parent)
    : QWidget(parent), mLibrary(library), mTitle(std::move(title))
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(3);
    mThumb = new QToolButton(this);
    mThumb->setObjectName("imageThumb");
    mThumb->setAutoRaise(true);
    mThumb->setIconSize(QSize(40, 26));
    mThumb->setToolTip("Pick an image: type to filter");
    mPath = new QLineEdit(this);
    mPath->setObjectName("imagePath");
    mPath->setPlaceholderText("None");
    mName = new QToolButton(this);
    mName->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mName->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mName->setToolTip("Pick an image: type to filter");
    mName->hide();
    mOpen = new QToolButton(this);
    mOpen->setArrowType(Qt::DownArrow);
    mOpen->setToolTip("Pick from the library and the project's textures, or Browse...; type to filter");
    mClear = new QToolButton(this);
    mClear->setIcon(icon("x"));
    mClear->setToolTip("None");
    row->addWidget(mThumb);
    row->addWidget(mPath, 1);
    row->addWidget(mName, 1);
    row->addWidget(mOpen);
    row->addWidget(mClear);
    // A click on the thumbnail waits out the double-click interval, since a double-click opens the image instead.
    mThumbClick = new QTimer(this);
    mThumbClick->setSingleShot(true);
    mThumbClick->setInterval(QApplication::doubleClickInterval());
    connect(mThumbClick, &QTimer::timeout, this, [this] { open(); });
    connect(mThumb, &QToolButton::clicked, this, [this] {
        if (mFile.empty())
            open();
        else
            mThumbClick->start();
    });
    for (QToolButton* b : {mName, mOpen})
        connect(b, &QToolButton::clicked, this, [this] { open(); });
    connect(mClear, &QToolButton::clicked, this, [this] { choose({}); });
    connect(mPath, &QLineEdit::editingFinished, this, [this] {
        if (!mPath->isModified())
            return;
        mPath->setModified(false);
        QString text = mPath->text().trimmed();
        if (text.size() >= 2 && text.startsWith('"') && text.endsWith('"'))
            text = text.mid(1, text.size() - 2);
        if (text == mShown)
            return;
        choose(text.toStdString());
    });

    if (mLibrary)
    {
        auto* manage = new QToolButton(this);
        manage->setIcon(icon("library-plus"));
        manage->setToolTip("Add " + mLibrary->title.toLower() + " to your library or the project's, or open their folders");
        manage->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(manage);
        const imagePicker::Library* lib = mLibrary;
        connect(menu->addAction("Add to My " + lib->title + "..."), &QAction::triggered, this, [this, lib] { addTo(lib->userDir); });
        QAction* addProject = menu->addAction("Add to Project " + lib->title + "...");
        connect(addProject, &QAction::triggered, this, [this, lib] { addTo(lib->projectDir ? lib->projectDir() : fs::path()); });
        menu->addSeparator();
        auto reveal = [](const fs::path& dir) {
            if (dir.empty())
                return;
            std::error_code ec;
            fs::create_directories(dir, ec);
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(paths::toUtf8(dir))));
        };
        connect(menu->addAction("Open My " + lib->title + " Folder"), &QAction::triggered, this, [lib, reveal] { reveal(lib->userDir); });
        QAction* openProject = menu->addAction("Open Project " + lib->title + " Folder");
        connect(openProject, &QAction::triggered, this, [lib, reveal] { reveal(lib->projectDir ? lib->projectDir() : fs::path()); });
        connect(menu, &QMenu::aboutToShow, this, [lib, addProject, openProject] {
            const bool project = lib->projectDir && !lib->projectDir().empty();
            addProject->setEnabled(project);
            openProject->setEnabled(project);
        });
        manage->setMenu(menu);
        row->addWidget(manage);
    }

    setAcceptDrops(true);
    for (QWidget* w : {static_cast<QWidget*>(this), static_cast<QWidget*>(mPath), static_cast<QWidget*>(mThumb), static_cast<QWidget*>(mName)})
        w->installEventFilter(this);
    refresh();
}

void ImagePicker::setValue(const std::string& value)
{
    if (value == mValue && !mShown.isNull())
        return;
    mValue = value;
    refresh();
}

void ImagePicker::setEditable(bool on)
{
    mEditable = on;
    mPath->setVisible(on);
    mName->setVisible(!on);
}

void ImagePicker::setClearable(bool on) { mClear->setVisible(on); }

void ImagePicker::setDrops(bool on) { mDrops = on; }

void ImagePicker::setDropMany(std::function<void(const std::vector<std::string>&)> fn) { mDropMany = std::move(fn); }

void ImagePicker::setResolver(std::function<fs::path(const std::string&)> fn)
{
    mResolve = std::move(fn);
    refresh();
}

void ImagePicker::setExtraItems(std::function<std::vector<texturePicker::Item>()> fn) { mExtraItems = std::move(fn); }

void ImagePicker::setProblem(std::function<std::string(const fs::path&)> fn)
{
    mProblem = std::move(fn);
    refresh();
}

void ImagePicker::choose(const std::string& value)
{
    mValue = value;
    refresh();
    Q_EMIT chosen(QString::fromStdString(value));
}

void ImagePicker::refresh()
{
    QString name, tip;
    QIcon thumb;
    mFile.clear();
    if (mValue.empty())
        name = QString();
    else
    {
        const imagePicker::Entry* found = nullptr;
        std::vector<imagePicker::Entry> entries;
        if (mLibrary && mLibrary->entries)
        {
            entries = mLibrary->entries();
            for (const imagePicker::Entry& e : entries)
                if (e.value == mValue)
                    found = &e;
        }
        const fs::path file = found ? found->file : mResolve ? mResolve(mValue) : paths::resolvePath(mValue);
        std::error_code ec;
        const bool exists = !file.empty() && fs::exists(file, ec);
        thumb = imagePicker::thumbnail(file);
        if (exists)
            mFile = file;
        // Library images show their name; files show their path, as typed or stored.
        name = found ? found->name : QString::fromStdString(mValue);
        const std::string problem = !exists ? std::string("missing") : mProblem ? mProblem(file) : std::string();
        tip = QString::fromStdString(paths::toUtf8(file)) + (problem.empty() ? QString() : "\n" + QString::fromStdString(problem));
        if (!exists)
            name += " (missing)";
    }
    mShown = name.isNull() ? QString("") : name;
    if (!mPath->hasFocus())
    {
        mPath->setText(mShown);
        mPath->setModified(false);
    }
    mPath->setToolTip(tip);
    mName->setText(mValue.empty() ? QString("None") : name);
    mName->setToolTip(tip);
    mThumb->setIcon(thumb.isNull() ? icon(mValue.empty() ? "photo-off" : "photo-x") : thumb);
    mThumb->setToolTip(tip.isEmpty() ? QString("Pick an image: type to filter") : tip + "\nDouble-click to open, right-click for more");
    mClear->setEnabled(!mValue.empty());
}

bool ImagePicker::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type())
    {
    case QEvent::MouseButtonDblClick:
        if (watched != mThumb || static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton || mFile.empty())
            break;
        mThumbClick->stop();
        openFile();
        return true;
    case QEvent::ContextMenu:
        if (watched != mThumb && watched != mName)
            break;
        contextMenu()->popup(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    case QEvent::DragEnter:
    case QEvent::DragMove:
    {
        auto* e = static_cast<QDropEvent*>(event);
        if (!mDrops || !isEnabled() || droppedImageFiles(e->mimeData()).empty())
            break;
        e->acceptProposedAction();
        return true;
    }
    case QEvent::Drop:
    {
        auto* e = static_cast<QDropEvent*>(event);
        const std::vector<std::string> files = mDrops ? droppedImageFiles(e->mimeData()) : std::vector<std::string>();
        if (files.empty())
            break;
        e->acceptProposedAction();
        if (files.size() > 1 && mDropMany)
            mDropMany(files);
        else
            choose(paths::storedPath(paths::fromUtf8(files.front())));
        return true;
    }
    default: break;
    }
    return QWidget::eventFilter(watched, event);
}

std::vector<std::string> droppedImageFiles(const QMimeData* mime)
{
    std::vector<std::string> files;
    if (!mime || !mime->hasUrls())
        return files;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile())
            if (const fs::path p(url.toLocalFile().toStdU16String()); imagePicker::isImageFile(p))
                files.push_back(paths::toUtf8(fs::absolute(p)));
    return files;
}

void ImagePicker::open()
{
    texturePicker::Options o;
    o.current = mValue;
    o.title = mTitle;
    o.favourites = "images/" + (mLibrary ? mLibrary->title : QString("Texture")).toStdString();
    if (mLibrary)
    {
        if (mLibrary->entries)
            o.items = [lib = mLibrary] {
                std::vector<texturePicker::Item> out;
                for (const imagePicker::Entry& e : lib->entries())
                    out.push_back({e.value, e.name, e.file, e.group});
                return out;
            };
        o.projectTextures = mLibrary->projectTextures;
        o.extensions = mLibrary->extensions;
        o.browseFilter = mLibrary->filter;
        o.actions.emplace_back("Add to My " + mLibrary->title + "...", [this] { addTo(mLibrary->userDir); });
        if (mLibrary->projectDir && !mLibrary->projectDir().empty())
            o.actions.emplace_back("Add to Project " + mLibrary->title + "...", [this] { addTo(mLibrary->projectDir()); });
    }
    else
        o.browseFilter = kImageFiles;
    if (mExtraItems)
        o.items = [extra = mExtraItems, library = std::move(o.items)] {
            std::vector<texturePicker::Item> out = extra();
            if (library)
                for (texturePicker::Item& i : library())
                    out.push_back(std::move(i));
            return out;
        };
    QPointer<ImagePicker> self(this);
    texturePicker::open(this, std::move(o), [self](const std::string& value) {
        if (!self)
            return;
        // Files from the project or a browse are stored as the scene stores paths; library values as they are.
        std::error_code ec;
        const bool file = !value.empty() && fs::path(paths::fromUtf8(value)).is_absolute() && fs::exists(paths::fromUtf8(value), ec);
        self->choose(file ? paths::storedPath(paths::fromUtf8(value)) : value);
    });
}

void ImagePicker::openFile() const
{
    if (!mFile.empty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(paths::toUtf8(mFile))));
}

QMenu* ImagePicker::contextMenu()
{
    auto* m = new QMenu(this);
    m->setObjectName("imagePickerMenu");
    m->setAttribute(Qt::WA_DeleteOnClose);
    const bool has = !mFile.empty();
    const QString file = QString::fromStdString(paths::toUtf8(mFile));
    m->addAction(icon("external-link"), "Open", m, [this] { openFile(); })->setEnabled(has);
    m->addAction(icon("folder-search"), "Open Containing Folder", m, [file] { fileOps::reveal(file); })->setEnabled(has);
    m->addAction(icon("copy"), "Copy Path", m, [file] { QApplication::clipboard()->setText(QDir::toNativeSeparators(file)); })->setEnabled(has);
    m->addSeparator();
    m->addAction("Pick...", m, [this] { open(); });
    if (mClear->isVisibleTo(this))
        m->addAction(icon("x"), "None", m, [this] { choose({}); })->setEnabled(!mValue.empty());
    return m;
}

void ImagePicker::addTo(const fs::path& dir)
{
    if (dir.empty() || !mLibrary)
        return;
    const QStringList files = fileDialog::getOpenFileNames(this, "Add " + mLibrary->title, {}, mLibrary->filter.isEmpty() ? kImageFiles : mLibrary->filter);
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path last;
    for (const QString& f : files)
    {
        const fs::path src(f.toStdU16String());
        fs::path dst = dir / src.filename();
        for (int n = 2; fs::exists(dst, ec); ++n)
            dst = dir / (paths::fromUtf8(paths::toUtf8(src.stem()) + "_" + std::to_string(n)).native() + src.extension().native());
        if (!fs::copy_file(src, dst, ec))
            log::error("Add {}: {}", paths::toUtf8(src), ec.message());
        else
            last = dst;
    }
    if (!last.empty())
        choose(mLibrary->valueFor ? mLibrary->valueFor(last) : paths::storedPath(last));
}
} // namespace lila::ui
