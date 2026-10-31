#include "ToolPdfCombocontrol.h"

#include <utility>  // for move

#include "control/ToolEnums.h"
#include "control/actions/ActionDatabase.h"
#include "gui/IconNameHelper.h"
#include "util/i18n.h"

ToolPdfCombocontrol::ToolPdfCombocontrol(std::string id, IconNameHelper& icons, const ActionDatabase& db):
        ComboToolButton(
                std::move(id), Category::SELECTION, icons.iconName("copy-pdf-area"),
                _("PDF text selection dropdown menu"),
                // Fork: copying an area is the default (the first entry), the text selections follow
                {Entry(_("Copy PDF Area"), icons.iconName("copy-pdf-area"), TOOL_COPY_PDF_AREA),
                 Entry(_("Select Linear PDF Text"), icons.iconName("select-pdf-text-ht"), TOOL_SELECT_PDF_TEXT_LINEAR),
                 Entry(_("Select PDF Text In Rectangle"), icons.iconName("select-pdf-text-area"),
                       TOOL_SELECT_PDF_TEXT_RECT)},
                db.getAction(Action::SELECT_TOOL)) {}
