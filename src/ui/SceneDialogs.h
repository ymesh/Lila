#pragma once

#include "core/Export.h"

#include "io/Examples.h"
#include "io/Samples.h"
#include "io/Recovery.h"
#include "ui/WelcomeNews.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <vector>

class QCheckBox;
class QHBoxLayout;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace lila::ui
{
LILA_UI_API void openCrashReport(const std::filesystem::path& dir);
LILA_UI_API void openCrashFolder(const std::filesystem::path& dir);
/** Puts report.txt on the clipboard. */
LILA_UI_API void copyCrashReport(const std::filesystem::path& dir);

/**
 * After a crash: what crashed (with the report a click away) when crashReport is set, and the scenes with newer
 * autosaves or emergency saves, with what to do with the selected one.
 */
class LILA_UI_API RecoveryDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Choice
    {
        Discard,
        Recover,
        OpenSaved,
    };

    RecoveryDialog(const std::vector<io::recovery::Candidate>& candidates, const std::filesystem::path& crashReport, QWidget* parent);

    Choice choice() const { return mChoice; }
    /** The candidate the choice applies to; only when there are candidates. */
    const io::recovery::Candidate& selected() const;

private:
    void finish(Choice c);

    std::vector<io::recovery::Candidate> mCandidates;
    QTreeWidget* mList = nullptr;
    QPushButton* mOpenSaved = nullptr;
    Choice mChoice = Choice::Discard;
};

inline constexpr const char* kShowWelcomePref = "/startup/showWelcome";
/** Which Legendary supporter the welcome screen names next. */
inline constexpr const char* kCreditsRotationPref = "/startup/creditsRotation";
/** The recent scenes on their own at startup, when the welcome dialog is off. */
inline constexpr const char* kShowRecentPref = "/startup/showDialog";

/**
 * The frameless startup picker. welcome: the title and Essentials button over two zones, New Scene, Open and the recent
 * scenes down the left, the latest news over the samples and examples on the right; otherwise the recent scenes alone.
 * Enter or double-click opens, Esc dismisses from any of the app's windows; a click outside also dismisses the recent
 * scenes alone, not the welcome.
 */
class LILA_UI_API StartupDialog : public QDialog
{
    Q_OBJECT

public:
    struct Hooks
    {
        std::function<void(const std::filesystem::path&)> open; // empty path = browse
        std::function<void(const io::examples::Example&)> openExample;
        std::function<void(const io::samples::Sample&)> openSample; // an installed sample
        std::function<bool()> mayReplaceScene;                       // asks to discard unsaved changes
        std::function<void()> newScene;
    };

    StartupDialog(Hooks hooks, bool welcome, QWidget* parent);

    /** Centered on the screen parent is on. */
    void present();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void openSelected(QTreeWidget* list);
    void openExampleSelected();
    void openSampleSelected();
    void showRecentMenu(const QPoint& at);
    void showExampleMenu(const QPoint& at);
    /** Moves a downloaded sample to the trash, with every sample sharing its download, and badges them for download again. */
    void trashSample(size_t index);
    /** Drops the scene from the recent files pref and the list. */
    void removeRecent(QTreeWidgetItem* item);
    /**
     * The middle column: the news card, opening on the newest item not yet seen, and under it the showcase entry shown
     * fewest times, or a placeholder while the feed has none running.
     */
    void showNews(const std::vector<news::Item>& items);
    /** Shows the news item at mNewsShown. */
    void showNewsItem();
    QWidget* makeShowcasePlaceholder();
    /** The examples list's width for that many cards across. */
    int examplesWidth(int across) const;

    Hooks mHooks;
    QWidget* mNews = nullptr;
    QVBoxLayout* mNewsBand = nullptr;
    QPointer<QWidget> mNewsCard;
    QPointer<QWidget> mShowcaseCard;
    std::vector<std::string> mSeenBefore;
    bool mSeenLoaded = false;
    std::string mShowcaseId;
    std::vector<news::Item> mNewsItems;
    size_t mNewsShown = 0;
    QTreeWidget* mList = nullptr;
    QListWidget* mExamples = nullptr;
    std::vector<io::examples::Example> mExampleList;
    std::vector<io::samples::Sample> mSampleList;
    QElapsedTimer mReclaimFocus;
    bool mWelcome = false;
};

/** File > Versions: everything Lila and source control kept of the current scene. */
class LILA_UI_API VersionsDialog : public QDialog
{
    Q_OBJECT

public:
    struct Hooks
    {
        /** Unsaved-changes prompt; false = cancelled. */
        std::function<bool()> confirmDiscard;
        std::function<void()> opened;
    };

    VersionsDialog(const std::filesystem::path& scene, Hooks hooks, QWidget* parent);

private:
    void populate();
    void addRevisions(const nlohmann::json& revisions);
    void act(bool restore);
    void load(const std::filesystem::path& file, bool restore, const QString& label);

    std::filesystem::path mScene;
    Hooks mHooks;
    QTreeWidget* mList = nullptr;
    QPushButton* mOpen = nullptr;
    QPushButton* mRestore = nullptr;
};
} // namespace lila::ui
