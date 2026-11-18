#pragma once

#include "core/Export.h"
#include "ui/TexturePicker.h"

#include <QIcon>
#include <QImage>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QLineEdit;
class QMenu;
class QTimer;
class QMimeData;
class QToolButton;

namespace lila::scene
{
struct NodeType;
}

namespace lila::ui
{
/**
 * Images an image attr picks from: Lila's, the user's and the project's, each a folder. An attr is tied to one with
 * imagePicker::setLibraryFor; light.texture uses "hdri".
 */
namespace imagePicker
{
struct Entry
{
    /** What the attr stores: a bare file name for Lila's own (resolved by the attr's reader), else a path. */
    std::string value;
    QString name;
    std::filesystem::path file;
    QString group;
};

struct Library
{
    std::string id;
    /** "HDRIs": the add and open menu items name it. */
    QString title;
    std::function<std::vector<Entry>()> entries;
    std::filesystem::path userDir;
    /** Empty without an open project. */
    std::function<std::filesystem::path()> projectDir;
    QString filter;
    /** Also list the project's textures, limited to these lowercase extensions (".hdr"; empty: every image). */
    bool projectTextures = true;
    std::vector<std::string> extensions;
    /** What a file added to the library's folders stores; empty stores its path. */
    std::function<std::string(const std::filesystem::path&)> valueFor;
};

/** Adds or replaces a library. "hdri" is built in. */
LILA_UI_API void addLibrary(Library library);
LILA_UI_API const Library* library(std::string_view id);
/** Ties nodeType.attr (and the same attr on derived types) to a library. */
LILA_UI_API void setLibraryFor(std::string nodeType, std::string attr, std::string library);
LILA_UI_API const Library* libraryFor(const scene::NodeType& type, std::string_view attr);

/** An image file by its extension (PNG, JPEG, TGA, HDR, EXR, BMP, TIFF, PSD, WebP). */
LILA_UI_API bool isImageFile(const std::filesystem::path& file);
/** A thumbnail of an image file (HDRs tone mapped), size pixels on its long side; cached by file and modification time. Null when unreadable. */
LILA_UI_API QIcon thumbnail(const std::filesystem::path& file, int size = 48);
/** The same thumbnail as an image, uncached; safe off the main thread. */
LILA_UI_API QImage thumbnailImage(const std::filesystem::path& file, int size);
} // namespace imagePicker

/**
 * Lila's image field, wherever a texture is picked (image attrs such as the dome light, material maps, texture graph
 * images, brush alphas): the image's thumbnail, its path to type or paste over, a button opening texturePicker (None,
 * the library by group, the project's textures, Browse...; type to filter), a clear button, image files dropped on it,
 * and with a library a button to add images to it or open its folders. Double-clicking the thumbnail opens the image
 * in the system's viewer; right-clicking it or the name lists Open, Open Containing Folder, Copy Path, Pick and None.
 * Each part can be switched off.
 */
class LILA_UI_API ImagePicker final : public QWidget
{
    Q_OBJECT
public:
    /** library may be null: then the popup lists the project's textures. */
    ImagePicker(const imagePicker::Library* library, QString title, QWidget* parent = nullptr);

    /** Shows value as the current choice without emitting chosen. */
    void setValue(const std::string& value);
    const std::string& value() const { return mValue; }

    /** Typing or pasting a path (on by default); off shows the name only. */
    void setEditable(bool on);
    /** The clear button (on by default). */
    void setClearable(bool on);
    /** Image files dropped on the field (on by default). One file is chosen; several go to dropMany when set. */
    void setDrops(bool on);
    void setDropMany(std::function<void(const std::vector<std::string>& files)> fn);
    /** Turns a stored value into the file to show (default paths::resolvePath), e.g. materials' texture search. */
    void setResolver(std::function<std::filesystem::path(const std::string&)> fn);
    /** What is wrong with a resolved file, shown on the thumbnail; empty when fine. */
    void setProblem(std::function<std::string(const std::filesystem::path&)> fn);
    /** More rows listed first in the picker popup, asked each time it opens (a material map's texture sets). */
    void setExtraItems(std::function<std::vector<texturePicker::Item>()> fn);

Q_SIGNALS:
    void chosen(const QString& value);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    void open();
    void addTo(const std::filesystem::path& dir);
    void choose(const std::string& value);
    void openFile() const;
    QMenu* contextMenu();

    const imagePicker::Library* mLibrary;
    QString mTitle;
    QToolButton* mThumb;
    QLineEdit* mPath;
    QToolButton* mName;
    QToolButton* mOpen;
    QToolButton* mClear;
    QTimer* mThumbClick;
    std::string mValue;
    /** The resolved file of the value; empty when unset or missing. */
    std::filesystem::path mFile;
    /** What the path box shows for the value, to tell an edit from an untouched name. */
    QString mShown;
    bool mEditable = true;
    bool mDrops = true;
    std::function<void(const std::vector<std::string>&)> mDropMany;
    std::function<std::filesystem::path(const std::string&)> mResolve;
    std::function<std::string(const std::filesystem::path&)> mProblem;
    std::function<std::vector<texturePicker::Item>()> mExtraItems;
};

/** The image files among dropped data (local files with an image extension), absolute paths. */
LILA_UI_API std::vector<std::string> droppedImageFiles(const QMimeData* mime);
} // namespace lila::ui
