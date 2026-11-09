#include "SearchBar.h"

#include <string>  // for allocator, string

#include <gdk/gdk.h>         // for GdkEventKey, GDK_SHIFT_MASK
#include <gdk/gdkkeysyms.h>  // for GDK_KEY_Return
#include <glib-object.h>     // for G_CALLBACK, g_signal_connect
#include <glib.h>            // for g_free, g_strdup_printf

#include "control/Control.h"            // for Control
#include "control/NavigationHistory.h"  // for NavigationHistory
#include "control/ScrollHandler.h"      // for ScrollHandler
#include "control/zoom/ZoomControl.h"  // for ZoomControl
#include "gui/MainWindow.h"            // for MainWindow
#include "model/Document.h"            // for Document
#include "util/PlaceholderString.h"    // for PlaceholderString
#include "util/i18n.h"                 // for _, FC, _F

SearchBar::SearchBar(Control* control): control(control) {
    MainWindow* win = control->getWindow();

    GtkWidget* close = win->get("buttonCloseSearch");
    g_signal_connect(close, "clicked", G_CALLBACK(buttonCloseSearchClicked), this);

    GtkWidget* next = win->get("btSearchForward");
    GtkWidget* previous = win->get("btSearchBack");
    g_signal_connect(next, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<SearchBar*>(d)->searchNext(); }), this);
    g_signal_connect(previous, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<SearchBar*>(d)->searchPrevious(); }), this);

    GtkWidget* searchTextField = win->get("searchTextField");
    g_signal_connect(searchTextField, "search-changed", G_CALLBACK(searchTextChangedCallback), this);
    g_signal_connect(searchTextField, "stop-search",
                     G_CALLBACK(+[](GtkSearchEntry*, gpointer d) { static_cast<SearchBar*>(d)->showSearchBar(false); }),
                     this);
    g_signal_connect(searchTextField, "next-match", G_CALLBACK(+[](GtkSearchEntry* e, gpointer d) {
                         static_cast<SearchBar*>(d)->searchNext();
                         // Grab focus again since searching will take away focus
                         gtk_widget_grab_focus(GTK_WIDGET(e));
                     }),
                     this);
    g_signal_connect(searchTextField, "previous-match", G_CALLBACK(+[](GtkSearchEntry* e, gpointer d) {
                         static_cast<SearchBar*>(d)->searchPrevious();
                         // Grab focus again since searching will take away focus
                         gtk_widget_grab_focus(GTK_WIDGET(e));
                     }),
                     this);
#if GTK_MAJOR_VERSION == 3
    GtkBindingSet* bindingSet = gtk_binding_set_by_class(GTK_SEARCH_ENTRY_GET_CLASS(searchTextField));
    gtk_binding_entry_add_signal(bindingSet, GDK_KEY_Return, GdkModifierType(0), "next-match", 0);
    gtk_binding_entry_add_signal(bindingSet, GDK_KEY_Return, GDK_SHIFT_MASK, "previous-match", 0);
#else
    GtkEventController* ctrl = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(ctrl),
                                         gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Return, GdkModifierType(0)),
                                                          gtk_signal_action_new("next-match")));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(ctrl),
                                         gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Return, GDK_SHIFT_MASK),
                                                          gtk_signal_action_new("previous-match")));
    gtk_widget_add_controller(searchTextField, ctrl);
#endif

    cssTextFild = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(win->get("searchTextField")),
                                   GTK_STYLE_PROVIDER(cssTextFild), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

SearchBar::~SearchBar() { this->control = nullptr; }

auto SearchBar::searchTextOnCurrentPage(const char* text, size_t index, size_t* occurrences, XojPdfRectangle* matchRect)
        -> bool {
    size_t p = control->getCurrentPageNo();
    this->page = p;

    return control->searchTextOnPage(text, p, index, occurrences, matchRect);
}

void SearchBar::search(const char* text) {
    MainWindow* win = control->getWindow();
    GtkWidget* lbSearchState = win->get("lbSearchState");

    bool found = true;
    this->indexInPage = 0;

    if (*text != 0) {
        found = searchTextOnCurrentPage(text, 1, &this->occurrences, nullptr);
        if (!found) {
            // Fork: search as you type finds the text on the following pages too, and shows the first match
            this->indexInPage = 0;
            searchNext();
            found = this->lastFound;
        } else if (found) {
            if (occurrences == 1) {
                gtk_label_set_text(GTK_LABEL(lbSearchState), _("Text found once on this page"));
            } else {
                char* msg = g_strdup_printf(_("Text found %zu times on this page"), occurrences);
                gtk_label_set_text(GTK_LABEL(lbSearchState), msg);
                g_free(msg);
            }
        } else {
            gtk_label_set_text(GTK_LABEL(lbSearchState), _("Text not found"));
        }
    } else {
        searchTextOnCurrentPage("", 1, nullptr, nullptr);
        gtk_label_set_text(GTK_LABEL(lbSearchState), "");
    }

    if (found) {
        gtk_css_provider_load_from_data(cssTextFild, "entry {}", -1, nullptr);
    } else {
        gtk_css_provider_load_from_data(cssTextFild, "entry { color: #ff0000; }", -1, nullptr);
    }
}

void SearchBar::searchTextChangedCallback(GtkSearchEntry* entry, SearchBar* searchBar) {
    searchBar->searchActive = false;
    const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
    searchBar->search(text);
}

void SearchBar::buttonCloseSearchClicked(GtkButton* button, SearchBar* searchBar) { searchBar->showSearchBar(false); }

template <class Fun>
void SearchBar::search(Fun next) {

    MainWindow* win = control->getWindow();
    GtkWidget* searchTextField = win->get("searchTextField");
    const char* text = gtk_entry_get_text(GTK_ENTRY(searchTextField));
    GtkWidget* lbSearchState = win->get("lbSearchState");
    if (*text == 0) {
        this->lastFound = false;
        return;
    }
    const size_t originalPage = page;

    XojPdfRectangle matchRect = XojPdfRectangle();
    // Search backwards through the pages, wrapping around if needed.
    for (;;) {
        next(text);
        const bool found = control->searchTextOnPage(text, page, indexInPage, &occurrences, &matchRect);

        if (found) {
            if (!searchActive) {
                control->getNavigationHistory()->recordNavPoint();
                searchActive = true;
            }
            control->getScrollHandler()->scrollToPage(page, matchRect);
            this->lastFound = true;
            gtk_label_set_text(GTK_LABEL(lbSearchState),
                               (occurrences == 1 ? FC(_F("Text found once on page {1}") % (page + 1)) :
                                                   FC(_F("Text found {1} times on page {2} ({3} of {1})") %
                                                      occurrences % (page + 1) % indexInPage)));
            return;
        }
        if (page == originalPage) {
            gtk_label_set_text(GTK_LABEL(lbSearchState), _("Text not found, searched on all pages"));
            this->lastFound = false;
            return;
        }
    }
}

void SearchBar::continueFromCurrentPage(bool forward) {
    // Fork: after scrolling elsewhere, Next and Previous go on from the page shown, not from where the search began
    const size_t current = control->getCurrentPageNo();
    if (current == this->page) {
        return;
    }
    MainWindow* win = control->getWindow();
    const char* text = gtk_entry_get_text(GTK_ENTRY(win->get("searchTextField")));
    control->searchTextOnPage(text, this->page, 1, &occurrences, nullptr);  // clears the active marker there
    const size_t pageCount = control->getDocument()->getPageCount();
    if (forward) {
        this->page = current;
        control->searchTextOnPage(text, this->page, 1, &occurrences, nullptr);
        this->indexInPage = 0;  // the next match is the first one on this page
    } else {
        // Previous goes to the last match of this page: start "after" it, on the following page
        this->page = current + 1 < pageCount ? current + 1 : 0;
        this->indexInPage = 1;
        control->searchTextOnPage(text, this->page, 1, &occurrences, nullptr);
    }
}

void SearchBar::searchNext() {
    continueFromCurrentPage(true);
    size_t pageCount = control->getDocument()->getPageCount();
    search([&](const char* text) {
        indexInPage++;
        if (indexInPage > occurrences) {
            control->searchTextOnPage(text, page, 1, &occurrences, nullptr);  // clear the active marker
            page++;
            if (page >= pageCount) {
                page = 0;
            }
            indexInPage = 1;
        }
    });
}

void SearchBar::searchPrevious() {
    continueFromCurrentPage(false);
    size_t pageCount = control->getDocument()->getPageCount();
    search([&](const char* text) {
        indexInPage--;
        if (indexInPage == 0 || indexInPage >= occurrences) {
            control->searchTextOnPage(text, page, 1, &occurrences, nullptr);  // clear the active marker
            page--;
            if (page > pageCount) {
                page = pageCount - 1;
            }
            control->searchTextOnPage(text, page, 1, &occurrences, nullptr);
            indexInPage = occurrences;
        }
    });
}

void SearchBar::showSearchBar(bool show) {
    MainWindow* win = control->getWindow();
    GtkWidget* searchBar = win->get("searchBar");

    if (show) {
        GtkWidget* searchTextField = win->get("searchTextField");
        gtk_widget_show_all(searchBar);
        gtk_widget_grab_focus(searchTextField);
        this->indexInPage = 0;
        // Fork: the previous text is selected (typing replaces it) and searched again from the page shown
        gtk_editable_select_region(GTK_EDITABLE(searchTextField), 0, -1);
        if (const char* text = gtk_entry_get_text(GTK_ENTRY(searchTextField)); text && *text) {
            this->page = control->getCurrentPageNo();
            search(text);
        }
    } else {
        searchActive = false;
        gtk_widget_hide(searchBar);
        const size_t pageCount = control->getDocument()->getPageCount();
        for (size_t i = pageCount - 1; i < pageCount; i--) {
            control->searchTextOnPage("", i, 0, nullptr, nullptr);
        }
    }
}
