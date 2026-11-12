#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

class QMainWindow;
class QWidget;

/**
 * Help > Report a Bug and Request a Feature file an issue on github.com/lila-3d/issues through the telemetry Worker, with the support bundle
 * and a screenshot as private attachments. Help > My Reports lists the reports sent from this machine with their status
 * and maintainer replies, and sends the reporter's answers, so reporters need no GitHub account.
 */
namespace lila::ui::issueReport
{
/** Status shown for a report: its triage label (Pending, Needs Info, On Hold, Solved, Dismissed), else Open or Closed. */
LILA_UI_API std::string statusText(const std::string& state, const std::vector<std::string>& labels);

/** Opens the report, or raises the open one, as it was left. */
LILA_UI_API void showDialog(QMainWindow* window);
/** Opens the report as a Bug or a Feature Request; a draft that already has a title keeps its type. */
LILA_UI_API void showBug(QMainWindow* window);
LILA_UI_API void showRequest(QMainWindow* window);
/** Opens the report as a Crash about `crashDir`, once any modal dialog (Lila Crashed) has closed. */
LILA_UI_API void showCrash(QMainWindow* window, const std::filesystem::path& crashDir);
LILA_UI_API void showMyReports(QWidget* parent);

/** Reopens a report that was open when Lila closed, and checks sent reports for new replies a little later. */
LILA_UI_API void startup(QMainWindow* window);
} // namespace lila::ui::issueReport
