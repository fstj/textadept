// Copyright 2007-2022 Mitchell. See LICENSE.
// GTK platform for Textadept.

#include "textadept.h"

#include "lauxlib.h"

#if __linux__
#include <signal.h>
#include <sys/wait.h>
#elif _WIN32
#include <fcntl.h>
#include <windows.h>
#define main main_ // entry point should be WinMain
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#if __APPLE__
#include <gtkmacintegration/gtkosxapplication.h>
#endif
#include "ScintillaWidget.h" // must come after <gtk/gtk.h>

// Translate GTK 2.x API to GTK 3.0 for compatibility.
#if GTK_CHECK_VERSION(3, 0, 0)
#define gtk_combo_box_entry_new_with_model(m, _) gtk_combo_box_new_with_model_and_entry(m)
#define gtk_combo_box_entry_set_text_column gtk_combo_box_set_entry_text_column
#define GTK_COMBO_BOX_ENTRY GTK_COMBO_BOX
#endif
#if !GTK_CHECK_VERSION(3, 22, 0)
#define GtkFileChooserNative GtkWidget
#define gtk_file_chooser_native_new(t, p, m, a, c) \
  gtk_file_chooser_dialog_new(t, p, m, c, GTK_RESPONSE_CANCEL, a, GTK_RESPONSE_ACCEPT, NULL)
#define GtkNativeDialog GtkDialog
#define GTK_NATIVE_DIALOG GTK_DIALOG
#define gtk_native_dialog_run gtk_dialog_run
#define gtk_native_dialog_destroy(w) gtk_widget_destroy(GTK_WIDGET(w))
#endif

// GTK objects.
static GtkWidget *window, *menubar, *tabbar, *statusbar[2];
static GtkAccelGroup *accel;
#if __APPLE__
static GtkosxApplication *osxapp;
#endif
static GtkWidget *findbox, *find_entry, *repl_entry, *find_label, *repl_label;
static GtkListStore *find_history, *repl_history;

static bool tab_sync;
static int current_tab;

#if _WIN32
static const char *pipe_id = "\\\\.\\pipe\\textadept.editor"; // for single-instance functionality
#endif

const char *get_platform() { return "GTK"; }

const char *get_charset() {
  const char *charset;
  g_get_charset(&charset);
  return charset;
}

// Signal for exiting Textadept.
// Generates a 'quit' event. If that event does not return `true`, quits the application.
static bool exiting(GtkWidget *_, GdkEventAny *__, void *___) {
  if (emit("quit", -1)) return true; // halt
  return (close_textadept(), scintilla_release_resources(), gtk_main_quit(), false);
}

// Signal for a Textadept window focus change.
// Generates a 'focus' event.
static bool window_focused(GtkWidget *_, GdkEventFocus *__, void *___) {
  if (!is_command_entry_active()) emit("focus", -1);
  return false;
}

// Signal for window focus loss.
// Generates an 'unfocus' event.
static bool focus_lost(GtkWidget *_, GdkEvent *__, void *___) {
  return (emit("unfocus", -1), is_command_entry_active()); // keep focus if window is losing focus
}

// Signal for a Textadept window keypress (not a Scintilla keypress).
static bool window_keypress(GtkWidget *_, GdkEventKey *event, void *__) {
  if (event->keyval != GDK_KEY_Escape || !gtk_widget_get_visible(findbox) ||
    gtk_widget_has_focus(command_entry))
    return false;
  return (gtk_widget_hide(findbox), gtk_widget_grab_focus(focused_view), true);
}

#if __APPLE__
// Signal for opening files from macOS.
// Generates an 'appleevent_odoc' event for each document sent.
static bool open_file(GtkosxApplication *_, char *path, void *__) {
  return (emit("appleevent_odoc", LUA_TSTRING, path, -1), true);
}

// Signal for block terminating Textadept from macOS.
// Generates a 'quit' event. There is no way to avoid quitting the application.
static bool terminating(GtkosxApplication *_, void *__) { return emit("quit", -1); }

// Signal for terminating Textadept from macOS.
// Closes Textadept and releases resources.
static void terminate(GtkosxApplication *_, void *__) {
  close_textadept(), scintilla_release_resources(), g_object_unref(osxapp), gtk_main_quit();
}
#endif

// Signal for switching buffer tabs.
// When triggered by the user (i.e. not synchronizing the tabbar), switches to the specified
// buffer.
// Generates a 'tab_clicked' event.
static void tab_changed(GtkNotebook *_, GtkWidget *__, int tab_num, void *___) {
  current_tab = tab_num;
  if (!tab_sync) emit("tab_clicked", LUA_TNUMBER, tab_num + 1, LUA_TNUMBER, 1, -1);
}

// Signal for reordering tabs.
static void tab_reordered(GtkNotebook *_, GtkWidget *__, int tab_num, void *___) {
  move_buffer(current_tab + 1, tab_num + 1, false);
}

// Signal for a Find/Replace entry keypress.
static bool find_keypress(GtkWidget *widget, GdkEventKey *event, void *_) {
  if (event->keyval != GDK_KEY_Return) return false;
  FindButton *button = (event->state & GDK_SHIFT_MASK) == 0 ?
    (widget == find_entry ? find_next : replace) :
    (widget == find_entry ? find_prev : replace_all);
  return (find_clicked(button), true);
}

// Creates and returns for the findbox a new GtkComboBoxEntry, storing its GtkLabel, GtkEntry,
// and GtkListStore in the given pointers.
static GtkWidget *new_combo(GtkWidget **label, GtkWidget **entry, GtkListStore **history) {
  *label = gtk_label_new(""); // localized label text set later via Lua
  *history = gtk_list_store_new(1, G_TYPE_STRING);
  GtkWidget *combo = gtk_combo_box_entry_new_with_model(GTK_TREE_MODEL(*history), 0);
  g_object_unref(*history);
  gtk_combo_box_entry_set_text_column(GTK_COMBO_BOX_ENTRY(combo), 0);
  gtk_combo_box_set_focus_on_click(GTK_COMBO_BOX(combo), false);
  *entry = gtk_bin_get_child(GTK_BIN(combo));
  gtk_entry_set_text(GTK_ENTRY(*entry), " "),
    gtk_entry_set_text(GTK_ENTRY(*entry), ""); // initialize with non-NULL
  gtk_label_set_mnemonic_widget(GTK_LABEL(*label), *entry);
  g_signal_connect(*entry, "key-press-event", G_CALLBACK(find_keypress), NULL);
  return combo;
}

// Signal for a Find entry keypress.
// Generates a 'find_text_changed' event.
static void find_changed(GtkEditable *_, void *__) { emit("find_text_changed", -1); }

// Signal for a Find button click.
static void button_clicked(GtkWidget *button, void *_) { find_clicked(button); }

// Creates and returns a new button for the findbox.
static GtkWidget *new_button() {
  GtkWidget *button = gtk_button_new_with_mnemonic(""); // localized via Lua
  g_signal_connect(button, "clicked", G_CALLBACK(button_clicked), NULL);
  gtk_widget_set_can_focus(button, false);
  return button;
}

// Creates and returns a new checkbox option for the findbox.
static GtkWidget *new_option() {
  GtkWidget *option = gtk_check_button_new_with_mnemonic(""); // localized later
  gtk_widget_set_can_focus(option, false);
  return option;
}

// Creates the findbox.
static GtkWidget *new_findbox() {
  findbox = gtk_table_new(2, 6, false);

  GtkWidget *find_combo = new_combo(&find_label, &find_entry, &find_history),
            *replace_combo = new_combo(&repl_label, &repl_entry, &repl_history);
  g_signal_connect(find_entry, "changed", G_CALLBACK(find_changed), NULL);
  find_next = new_button(), find_prev = new_button(), replace = new_button(),
  replace_all = new_button(), match_case = new_option(), whole_word = new_option(),
  regex = new_option(), in_files = new_option();

  GtkTable *table = GTK_TABLE(findbox);
  int expand = GTK_FILL | GTK_EXPAND, shrink = GTK_FILL | GTK_SHRINK;
  gtk_table_attach(table, find_label, 0, 1, 0, 1, shrink, shrink, 5, 0);
  gtk_table_attach(table, repl_label, 0, 1, 1, 2, shrink, shrink, 5, 0);
  gtk_table_attach(table, find_combo, 1, 2, 0, 1, expand, shrink, 5, 0);
  gtk_table_attach(table, replace_combo, 1, 2, 1, 2, expand, shrink, 5, 0);
  gtk_table_attach(table, find_next, 2, 3, 0, 1, shrink, shrink, 0, 0);
  gtk_table_attach(table, find_prev, 3, 4, 0, 1, shrink, shrink, 0, 0);
  gtk_table_attach(table, replace, 2, 3, 1, 2, shrink, shrink, 0, 0);
  gtk_table_attach(table, replace_all, 3, 4, 1, 2, shrink, shrink, 0, 0);
  gtk_table_attach(table, match_case, 4, 5, 0, 1, shrink, shrink, 5, 0);
  gtk_table_attach(table, whole_word, 4, 5, 1, 2, shrink, shrink, 5, 0);
  gtk_table_attach(table, regex, 5, 6, 0, 1, shrink, shrink, 5, 0);
  gtk_table_attach(table, in_files, 5, 6, 1, 2, shrink, shrink, 5, 0);

  return findbox;
}

void new_window(SciObject *(*get_view)(void)) {
  gtk_window_set_default_icon_name("textadept");

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(window, "textadept");
  gtk_window_set_default_size(GTK_WINDOW(window), 1000, 600);
  g_signal_connect(window, "delete-event", G_CALLBACK(exiting), NULL);
  g_signal_connect(window, "focus-in-event", G_CALLBACK(window_focused), NULL);
  g_signal_connect(window, "focus-out-event", G_CALLBACK(focus_lost), NULL);
  g_signal_connect(window, "key-press-event", G_CALLBACK(window_keypress), NULL);
  accel = gtk_accel_group_new();

#if __APPLE__
  gtkosx_application_set_use_quartz_accelerators(osxapp, false);
  g_signal_connect(osxapp, "NSApplicationOpenFile", G_CALLBACK(open_file), NULL);
  g_signal_connect(osxapp, "NSApplicationBlockTermination", G_CALLBACK(terminating), NULL);
  g_signal_connect(osxapp, "NSApplicationWillTerminate", G_CALLBACK(terminate), NULL);
#endif

  GtkWidget *vbox = gtk_vbox_new(false, 0);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  menubar = gtk_menu_bar_new();
  gtk_box_pack_start(GTK_BOX(vbox), menubar, false, false, 0);

  tabbar = gtk_notebook_new();
  g_signal_connect(tabbar, "switch-page", G_CALLBACK(tab_changed), NULL);
  g_signal_connect(tabbar, "page-reordered", G_CALLBACK(tab_reordered), NULL);
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(tabbar), true);
  gtk_widget_set_can_focus(tabbar, false);
  gtk_box_pack_start(GTK_BOX(vbox), tabbar, false, false, 0);

  GtkWidget *paned = gtk_vpaned_new();
  gtk_box_pack_start(GTK_BOX(vbox), paned, true, true, 0);

  GtkWidget *vboxp = gtk_vbox_new(false, 0);
  gtk_paned_add1(GTK_PANED(paned), vboxp);

  GtkWidget *hbox = gtk_hbox_new(false, 0);
  gtk_box_pack_start(GTK_BOX(vboxp), hbox, true, true, 0);

  gtk_box_pack_start(GTK_BOX(hbox), get_view(), true, true, 0);
  gtk_widget_grab_focus(focused_view);

  gtk_paned_add2(GTK_PANED(paned), command_entry);
  gtk_container_child_set(GTK_CONTAINER(paned), command_entry, "shrink", false, NULL);

  gtk_box_pack_start(GTK_BOX(vboxp), new_findbox(), false, false, 5);

  GtkWidget *hboxs = gtk_hbox_new(false, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hboxs, false, false, 1);

  statusbar[0] = gtk_label_new(NULL), statusbar[1] = gtk_label_new(NULL);
  gtk_box_pack_start(GTK_BOX(hboxs), statusbar[0], true, true, 5);
  gtk_misc_set_alignment(GTK_MISC(statusbar[0]), 0, 0);
  gtk_box_pack_start(GTK_BOX(hboxs), statusbar[1], true, true, 5);
  gtk_misc_set_alignment(GTK_MISC(statusbar[1]), 1, 0);

  gtk_widget_show_all(window);
  gtk_widget_hide(menubar), gtk_widget_hide(tabbar), gtk_widget_hide(findbox),
    gtk_widget_hide(command_entry); // hide initially
}

void set_title(const char *title) { gtk_window_set_title(GTK_WINDOW(window), title); }

bool is_maximized() {
  return (gdk_window_get_state(gtk_widget_get_window(window)) & GDK_WINDOW_STATE_MAXIMIZED) > 0;
}

void set_maximized(bool maximize) {
  maximize ? gtk_window_maximize(GTK_WINDOW(window)) : gtk_window_unmaximize(GTK_WINDOW(window));
}

void get_size(int *width, int *height) { gtk_window_get_size(GTK_WINDOW(window), width, height); }

void set_size(int width, int height) { gtk_window_resize(GTK_WINDOW(window), width, height); }

// Signal for a Scintilla keypress.
// Note: cannot use bool return value due to modern i686-w64-mingw32-gcc issue.
static int keypress(GtkWidget *_, GdkEventKey *event, void *__) {
  return emit("keypress", LUA_TNUMBER, event->keyval, LUA_TBOOLEAN, event->state & GDK_SHIFT_MASK,
    LUA_TBOOLEAN, event->state & GDK_CONTROL_MASK, LUA_TBOOLEAN, event->state & GDK_MOD1_MASK,
    LUA_TBOOLEAN, event->state & GDK_META_MASK, LUA_TBOOLEAN, event->state & GDK_LOCK_MASK, -1);
}

// Signal for a Scintilla mouse click.
static bool mouse_clicked(GtkWidget *w, GdkEventButton *event, void *_) {
  if (w == command_entry || event->type != GDK_BUTTON_PRESS || event->button != 3) return false;
  return (show_context_menu("context_menu", event), true);
}

SciObject *new_scintilla(void (*notified)(SciObject *, int, SCNotification *, void *)) {
  SciObject *view = scintilla_new();
  gtk_widget_set_size_request(view, 1, 1); // minimum size
  if (notified) g_signal_connect(view, SCINTILLA_NOTIFY, G_CALLBACK(notified), NULL);
  g_signal_connect(view, "key-press-event", G_CALLBACK(keypress), NULL);
  g_signal_connect(view, "button-press-event", G_CALLBACK(mouse_clicked), NULL);
  return view;
}

void focus_view(SciObject *view) { gtk_widget_grab_focus(view), update_ui(); }

sptr_t SS(SciObject *view, int message, uptr_t wparam, sptr_t lparam) {
  return scintilla_send_message(SCINTILLA(view), message, wparam, lparam);
}

void split_view(SciObject *view, SciObject *view2, bool vertical) {
  GtkAllocation allocation;
  gtk_widget_get_allocation(view, &allocation);
  int middle = (vertical ? allocation.width : allocation.height) / 2;
  GtkWidget *parent = gtk_widget_get_parent(view);
  g_object_ref(view);
  gtk_container_remove(GTK_CONTAINER(parent), view);
  GtkWidget *pane = vertical ? gtk_hpaned_new() : gtk_vpaned_new();
  gtk_paned_add1(GTK_PANED(pane), view), gtk_paned_add2(GTK_PANED(pane), view2);
  gtk_container_add(GTK_CONTAINER(parent), pane);
  gtk_paned_set_position(GTK_PANED(pane), middle);
  gtk_widget_show_all(pane), update_ui(); // ensure view2 is painted
  g_object_unref(view);
}

// Removes all Scintilla views from the given pane and deletes them along with the child panes
// themselves.
static void remove_views(GtkPaned *pane, void (*delete_view)(SciObject *view)) {
  GtkWidget *child1 = gtk_paned_get_child1(pane), *child2 = gtk_paned_get_child2(pane);
  GTK_IS_PANED(child1) ? remove_views(GTK_PANED(child1), delete_view) : delete_view(child1);
  GTK_IS_PANED(child2) ? remove_views(GTK_PANED(child2), delete_view) : delete_view(child2);
}

bool unsplit_view(SciObject *view, void (*delete_view)(SciObject *view)) {
  GtkWidget *pane = gtk_widget_get_parent(view);
  if (!GTK_IS_PANED(pane)) return false;
  GtkWidget *other = gtk_paned_get_child1(GTK_PANED(pane)) != view ?
    gtk_paned_get_child1(GTK_PANED(pane)) :
    gtk_paned_get_child2(GTK_PANED(pane));
  g_object_ref(view), g_object_ref(other);
  gtk_container_remove(GTK_CONTAINER(pane), view);
  gtk_container_remove(GTK_CONTAINER(pane), other);
  GTK_IS_PANED(other) ? remove_views(GTK_PANED(other), delete_view) : delete_view(other);
  GtkWidget *parent = gtk_widget_get_parent(pane);
  gtk_container_remove(GTK_CONTAINER(parent), pane);
  if (GTK_IS_PANED(parent))
    !gtk_paned_get_child1(GTK_PANED(parent)) ? gtk_paned_add1(GTK_PANED(parent), view) :
                                               gtk_paned_add2(GTK_PANED(parent), view);
  else
    gtk_container_add(GTK_CONTAINER(parent), view);
  // gtk_widget_show_all(parent);
  gtk_widget_grab_focus(GTK_WIDGET(view));
  g_object_unref(view), g_object_unref(other);
  return true;
}

void delete_scintilla(SciObject *view) { gtk_widget_destroy(view); }

Pane *get_top_pane() {
  GtkWidget *pane = focused_view;
  while (GTK_IS_PANED(gtk_widget_get_parent(pane))) pane = gtk_widget_get_parent(pane);
  return pane;
}

PaneInfo get_pane_info(Pane *pane) {
  PaneInfo info = {GTK_IS_PANED(pane), false, pane, pane, NULL, NULL, 0};
  GtkPaned *p = GTK_PANED(pane);
  if (info.is_split)
    info.vertical =
      gtk_orientable_get_orientation(GTK_ORIENTABLE(pane)) == GTK_ORIENTATION_HORIZONTAL,
    info.child1 = gtk_paned_get_child1(p), info.child2 = gtk_paned_get_child2(p),
    info.size = gtk_paned_get_position(p);
  return info;
}

PaneInfo get_pane_info_from_view(SciObject *v) { return get_pane_info(gtk_widget_get_parent(v)); }

void set_pane_size(Pane *pane, int size) { gtk_paned_set_position(GTK_PANED(pane), size); }

void show_tabs(bool show) { gtk_widget_set_visible(tabbar, show); }

void add_tab() {
  GtkWidget *tab = gtk_vbox_new(false, 0); // placeholder in GtkNotebook
  tab_sync = true;
  int i = gtk_notebook_append_page(GTK_NOTEBOOK(tabbar), tab, NULL);
  gtk_widget_show(tab);
  gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(tabbar), tab, true);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(tabbar), i);
  tab_sync = false;
}

void set_tab(int index) {
  tab_sync = true, gtk_notebook_set_current_page(GTK_NOTEBOOK(tabbar), index), tab_sync = false;
}

// Signal for a tab label mouse click.
static bool tab_clicked(GtkWidget *label, GdkEventButton *event, void *_) {
  GtkNotebook *notebook = GTK_NOTEBOOK(tabbar);
  for (int i = 0; i < gtk_notebook_get_n_pages(notebook); i++) {
    GtkWidget *page = gtk_notebook_get_nth_page(notebook, i);
    if (label != gtk_notebook_get_tab_label(notebook, page)) continue;
    emit("tab_clicked", LUA_TNUMBER, i + 1, LUA_TNUMBER, event->button, LUA_TBOOLEAN,
      event->state & GDK_SHIFT_MASK, LUA_TBOOLEAN, event->state & GDK_CONTROL_MASK, LUA_TBOOLEAN,
      event->state & GDK_MOD1_MASK, LUA_TBOOLEAN, event->state & GDK_META_MASK, -1);
    if (event->button == 3) show_context_menu("tab_context_menu", event);
    break;
  }
  return true;
}

void set_tab_label(int index, const char *text) {
  GtkWidget *box = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(box), false);
  GtkWidget *label = gtk_label_new(text);
  gtk_container_add(GTK_CONTAINER(box), label), gtk_widget_show(label);
  GtkNotebook *notebook = GTK_NOTEBOOK(tabbar);
  gtk_notebook_set_tab_label(notebook, gtk_notebook_get_nth_page(notebook, index), box);
  g_signal_connect(box, "button-press-event", G_CALLBACK(tab_clicked), NULL);
}

void move_tab(int from, int to) {
  GtkNotebook *notebook = GTK_NOTEBOOK(tabbar);
  gtk_notebook_reorder_child(notebook, gtk_notebook_get_nth_page(notebook, from), current_tab = to);
}

void remove_tab(int index) { gtk_notebook_remove_page(GTK_NOTEBOOK(tabbar), index); }

const char *get_find_text() { return gtk_entry_get_text(GTK_ENTRY(find_entry)); }
const char *get_repl_text() { return gtk_entry_get_text(GTK_ENTRY(repl_entry)); }
void set_find_text(const char *text) { gtk_entry_set_text(GTK_ENTRY(find_entry), text); }
void set_repl_text(const char *text) { gtk_entry_set_text(GTK_ENTRY(repl_entry), text); }

// Adds the given text to the given list store.
// Note: GtkComboBoxEntry key navigation behaves contrary to command line history
// navigation. Down cycles from newer to older, and up cycles from older to newer. In order to
// mimic traditional command line history navigation, append to the list instead of prepending
// to it.
static void add_to_history(GtkListStore *store, const char *text) {
  int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), NULL);
  GtkTreeIter iter;
  if (n > 9)
    gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter),
      gtk_list_store_remove(store, &iter), n--; // keep 10 items
  char *last_text = NULL;
  if (n > 0)
    gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &iter, NULL, n - 1),
      gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, &last_text, -1);
  if (!last_text || strcmp(text, last_text) != 0)
    gtk_list_store_append(store, &iter), gtk_list_store_set(store, &iter, 0, text, -1);
  g_free(last_text);
}

void add_to_find_history(const char *text) { add_to_history(find_history, text); }
void add_to_repl_history(const char *text) { add_to_history(repl_history, text); }

void set_entry_font(const char *name) {
  PangoFontDescription *font = pango_font_description_from_string(name);
  gtk_widget_modify_font(find_entry, font), gtk_widget_modify_font(repl_entry, font);
  pango_font_description_free(font);
}
bool is_checked(FindOption *opt) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(opt)); }
void toggle(FindOption *opt, bool on) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(opt), on); }
void set_find_label(const char *s) { gtk_label_set_text_with_mnemonic(GTK_LABEL(find_label), s); }
void set_repl_label(const char *s) { gtk_label_set_text_with_mnemonic(GTK_LABEL(repl_label), s); }
void set_button_label(FindButton *btn, const char *s) { gtk_button_set_label(GTK_BUTTON(btn), s); }
void set_option_label(FindOption *opt, const char *s) { gtk_button_set_label(GTK_BUTTON(opt), s); }
void focus_find() {
  if (!gtk_widget_has_focus(find_entry) && !gtk_widget_has_focus(repl_entry))
    gtk_widget_show(findbox), gtk_widget_grab_focus(find_entry);
  else
    gtk_widget_hide(findbox), gtk_widget_grab_focus(focused_view);
}
bool is_find_active() { return gtk_widget_get_visible(findbox); }

void focus_command_entry() {
  if (!gtk_widget_get_visible(command_entry))
    gtk_widget_show(command_entry), gtk_widget_grab_focus(command_entry);
  else
    gtk_widget_hide(command_entry), gtk_widget_grab_focus(focused_view);
}
bool is_command_entry_active() { return gtk_widget_has_focus(command_entry); }
int get_command_entry_height() {
  GtkAllocation allocation;
  gtk_widget_get_allocation(command_entry, &allocation);
  return allocation.height;
}
void set_command_entry_height(int height) {
  GtkWidget *paned = gtk_widget_get_parent(command_entry);
  GtkAllocation allocation;
  gtk_widget_get_allocation(paned, &allocation);
  gtk_widget_set_size_request(command_entry, -1, height);
  gtk_paned_set_position(GTK_PANED(paned), allocation.height - height);
}

void set_statusbar_text(int i, const char *s) { gtk_label_set_text(GTK_LABEL(statusbar[i]), s); }

// Signal for a menu item click.
static void menu_clicked(GtkWidget *_, void *id) {
  emit("menu_clicked", LUA_TNUMBER, (int)(long)id, -1);
}

void *read_menu(lua_State *L, int index) {
  GtkWidget *menu = gtk_menu_new(), *submenu_root = NULL;
  if (lua_getfield(L, index, "title")) { // submenu title
    submenu_root = gtk_menu_item_new_with_mnemonic(lua_tostring(L, -1));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(submenu_root), menu);
  }
  lua_pop(L, 1); // title
  for (size_t i = 1; i <= lua_rawlen(L, index); lua_pop(L, 1), i++) {
    if (lua_rawgeti(L, -1, i) != LUA_TTABLE) continue; // popped on loop
    bool is_submenu = lua_getfield(L, -1, "title");
    if (lua_pop(L, 1), is_submenu) {
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), read_menu(L, -1));
      continue;
    }
    const char *label = (lua_rawgeti(L, -1, 1), lua_tostring(L, -1));
    if (lua_pop(L, 1), !label) continue;
    // Menu item table is of the form {label, id, key, modifiers}.
    GtkWidget *menu_item =
      *label ? gtk_menu_item_new_with_mnemonic(label) : gtk_separator_menu_item_new();
    if (*label && get_int_field(L, -1, 3) > 0) {
      int key = get_int_field(L, -1, 3), modifiers = get_int_field(L, -1, 4), gdk_mods = 0;
      if (modifiers & SCMOD_SHIFT) gdk_mods |= GDK_SHIFT_MASK;
      if (modifiers & SCMOD_CTRL) gdk_mods |= GDK_CONTROL_MASK;
      if (modifiers & SCMOD_ALT) gdk_mods |= GDK_MOD1_MASK;
      if (modifiers & SCMOD_META) gdk_mods |= GDK_META_MASK;
      gtk_widget_add_accelerator(menu_item, "activate", accel, key, gdk_mods, GTK_ACCEL_VISIBLE);
    }
    g_signal_connect(
      menu_item, "activate", G_CALLBACK(menu_clicked), (void *)(long)get_int_field(L, -1, 2));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
  }
  return !submenu_root ? menu : submenu_root;
}

void popup_menu(void *menu, void *userdata) {
  GdkEventButton *event = (GdkEventButton *)userdata;
  gtk_widget_show_all(menu);
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, event ? event->button : 0,
    gdk_event_get_time((GdkEvent *)event));
}

void set_menubar(lua_State *L, int index) {
#if __APPLE__
  // TODO: gtkosx_application_set_menu_bar does not like being called more than once in an app.
  // Random segfaults will happen after a second reset, even if menubar is g_object_ref/unrefed
  // properly.
  if (!lua_getglobal(L, "arg")) return;
#endif
  GtkWidget *new_menubar = gtk_menu_bar_new();
  for (size_t i = 1; i <= lua_rawlen(L, index); lua_pop(L, 1), i++)
    gtk_menu_shell_append(GTK_MENU_SHELL(new_menubar),
      (lua_rawgeti(L, index, i), lua_touserdata(L, -1))); // popped on loop
  GtkWidget *vbox = gtk_widget_get_parent(menubar);
  gtk_container_remove(GTK_CONTAINER(vbox), menubar);
  gtk_box_pack_start(GTK_BOX(vbox), menubar = new_menubar, false, false, 0);
  gtk_box_reorder_child(GTK_BOX(vbox), new_menubar, 0);
  if (lua_rawlen(L, index) > 0) gtk_widget_show_all(new_menubar);
#if __APPLE__
  gtkosx_application_set_menu_bar(osxapp, GTK_MENU_SHELL(new_menubar));
  gtk_widget_hide(new_menubar); // hide in window
#endif
}

char *get_clipboard_text(int *len) {
  char *text = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
  *len = text ? strlen(text) : 0;
  return text;
}

// Contains information about an active timeout.
typedef struct {
  bool (*f)(int *);
  int *refs;
} TimeoutData;

// Signal for a timeout.
static int timed_out(void *data_) {
  TimeoutData *data = (TimeoutData *)data_;
  bool repeat = data->f(data->refs);
  if (!repeat) free(data);
  return repeat;
}

bool add_timeout(double interval, bool (*f)(int *), int *refs) {
  TimeoutData *data = malloc(sizeof(TimeoutData));
  data->f = f, data->refs = refs;
  return (g_timeout_add(interval * 1000, timed_out, data), true);
}

void update_ui() {
#if !__APPLE__
  while (gtk_events_pending()) gtk_main_iteration();
#else
  // The idle event monitor created by os.spawn() on macOS is considered to be a pending event,
  // so use its provided registry key to help determine when there are no longer any non-idle
  // events pending.
  lua_pushboolean(lua, false), lua_setfield(lua, LUA_REGISTRYINDEX, "spawn_procs_polled");
  while (gtk_events_pending()) {
    bool polled =
      (lua_getfield(lua, LUA_REGISTRYINDEX, "spawn_procs_polled"), lua_toboolean(lua, -1));
    if (lua_pop(lua, 1), polled) break;
    gtk_main_iteration();
  }
#endif
}

// Returns a new message dialog with the specified title, icon, and buttons.
// This base dialog is not limited to showing messages. More widgets can be added to it.
static GtkWidget *new_dialog(DialogOptions *opts) {
  GtkWidget *dialog =
    gtk_message_dialog_new(GTK_WINDOW(window), 0, GTK_MESSAGE_OTHER, 0, "%s", opts->title);
  if (opts->icon) {
    GtkWidget *image = gtk_image_new_from_icon_name(opts->icon, GTK_ICON_SIZE_DIALOG);
    gtk_message_dialog_set_image(GTK_MESSAGE_DIALOG(dialog), image), gtk_widget_show(image);
  }
  if (opts->buttons[2]) gtk_dialog_add_button(GTK_DIALOG(dialog), opts->buttons[2], 3);
  if (opts->buttons[1]) gtk_dialog_add_button(GTK_DIALOG(dialog), opts->buttons[1], 2);
  gtk_dialog_add_button(GTK_DIALOG(dialog), opts->buttons[0], 1);
  return (gtk_dialog_set_default_response(GTK_DIALOG(dialog), 1), dialog);
}

int message_dialog(DialogOptions opts, lua_State *L) {
  GtkWidget *dialog = new_dialog(&opts);
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", opts.text);
  int button = gtk_dialog_run(GTK_DIALOG(dialog));
  return (gtk_widget_destroy(dialog), button > 0 ? (lua_pushinteger(L, button), 1) : 0);
}

int input_dialog(DialogOptions opts, lua_State *L) {
  GtkWidget *dialog = new_dialog(&opts), *entry = gtk_entry_new();
  GtkWidget *box = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(box), entry, false, true, 0), gtk_widget_show(entry);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
  if (opts.text) gtk_entry_set_text(GTK_ENTRY(entry), opts.text);
  int button = gtk_dialog_run(GTK_DIALOG(dialog));
  if (button < 1 || (button == 2 && !opts.return_button)) return (gtk_widget_destroy(dialog), 0);
  lua_pushstring(L, gtk_entry_get_text(GTK_ENTRY(entry)));
  if (opts.return_button) lua_pushinteger(L, button);
  return (gtk_widget_destroy(dialog), !opts.return_button ? 1 : 2);
}

// `ui.dialogs.open{...}` or `ui.dialogs.save{...}` Lua function.
static int open_save_dialog(DialogOptions *opts, lua_State *L, bool open) {
  int mode = open ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SAVE;
  const char *accept = open ? GTK_STOCK_OPEN : GTK_STOCK_SAVE;
  GtkFileChooserNative *dialog =
    gtk_file_chooser_native_new(opts->title, GTK_WINDOW(window), mode, accept, GTK_STOCK_CANCEL);
  GtkFileChooser *fc = GTK_FILE_CHOOSER(dialog);
  if (opts->dir) gtk_file_chooser_set_current_folder(fc, opts->dir);
  if (open) {
    if (opts->only_dirs) gtk_file_chooser_set_action(fc, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_file_chooser_set_select_multiple(fc, opts->multiple);
    if (opts->dir && opts->file) {
      lua_pushstring(L, opts->dir), lua_pushliteral(L, G_DIR_SEPARATOR_S),
        lua_pushstring(L, opts->file), lua_concat(L, 3);
      gtk_file_chooser_select_filename(fc, lua_tostring(L, -1));
    }
  } else {
    gtk_file_chooser_set_do_overwrite_confirmation(fc, true);
    if (opts->file) gtk_file_chooser_set_current_name(fc, opts->file);
  }

  if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT)
    return (gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog)), 0);
  lua_newtable(L); // note: will be placed by single value of opts->multiple is false
  GSList *filenames = gtk_file_chooser_get_filenames(fc), *f = filenames;
  for (int i = 1; f; f = f->next, i++) lua_pushstring(L, f->data), lua_rawseti(L, -2, i);
  g_slist_free_full(filenames, free);
  if (!opts->multiple) lua_rawgeti(L, -1, 1), lua_replace(L, -2); // single value
  return (gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog)), 1);
}

int open_dialog(DialogOptions opts, lua_State *L) { return open_save_dialog(&opts, L, true); }
int save_dialog(DialogOptions opts, lua_State *L) { return open_save_dialog(&opts, L, false); }

// Contains information about a currently active progress dialog.
typedef struct {
  GtkWidget *dialog, *bar;
  bool (*work)(void (*update)(double, const char *, void *), void *);
} ProgressData;

// Updates the given progressbar with the given percentage and text.
static void update(double percent, const char *text, void *bar) {
  if (percent >= 0)
    gtk_progress_bar_set_fraction(bar, 0.01 * percent);
  else
    gtk_progress_bar_pulse(bar);
  if (text) gtk_progress_bar_set_text(bar, text);
}

// Signal to update the progressbar by calling the provided work Lua function.
static int do_work(void *data_) {
  ProgressData *data = data_;
  bool repeat = data->work(update, data->bar);
  if (!repeat) g_signal_emit_by_name(data->dialog, "response", 0);
  while (gtk_events_pending()) gtk_main_iteration();
  return repeat;
}

int progress_dialog(
  DialogOptions opts, lua_State *L, bool (*work)(void (*)(double, const char *, void *), void *)) {
  GtkWidget *dialog = new_dialog(&opts), *bar = gtk_progress_bar_new();
  GtkWidget *box = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(box), bar, false, true, 0), gtk_widget_show(bar);
  if (opts.text) gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar), opts.text);
  ProgressData data = {dialog, bar, work};
  int progressbar_source = g_timeout_add(0, do_work, &data);
  int stopped = gtk_dialog_run(GTK_DIALOG(dialog));
  if (stopped) g_source_remove(progressbar_source);
  return (gtk_widget_destroy(dialog), stopped ? (lua_pushboolean(L, true), 1) : 0);
}

// Function for comparing the given search key with a list's item/row.
// Iterates over all space-separated words in the key, matching each word to the item/row
// case-insensitively  and sequentially. If all key words match, returns 0 on success, like strcmp.
static int matches(GtkTreeModel *model, int column, const char *key, GtkTreeIter *iter, void *_) {
  if (!*key) return 0; // true
  char *item;
  gtk_tree_model_get(model, iter, column, &item, -1);
  const char *match_pos = item;
  for (const char *s = key, *e = s;; s = e) {
    while (*e && *e != ' ') e++;
    bool match = false;
    for (const char *p = match_pos; *p; p++)
      if (g_strncasecmp(s, p, e - s) == 0) {
        match_pos = p + (e - s), match = true;
        break;
      }
    if (!match || !*e++) return (free(item), !match); // !match is false, !*e is true (like strcmp)
  }
}

// Function for determining whether a list's item/row should be shown.
// A list item/row should only be shown if it matches the current search key.
static int visible(GtkTreeModel *model, GtkTreeIter *iter, void *treeview) {
  const char *key = gtk_entry_get_text(gtk_tree_view_get_search_entry(treeview));
  return matches(model, gtk_tree_view_get_search_column(treeview), key, iter, NULL) == 0;
}

// Selects the first item in the given view if an item is not already selected.
// This is needed particularly when initially showing the list with no search key and after
// clearing the search key and refiltering.
static void select_first_item(GtkTreeView *view) {
  GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
  if (gtk_tree_selection_count_selected_rows(selection) > 0) return; // already selected
  GtkTreeIter iter;
  if (gtk_tree_model_get_iter_first(gtk_tree_view_get_model(view), &iter))
    gtk_tree_selection_select_iter(selection, &iter);
}

// Signal for showing and hiding list values/rows depending on the current search key.
static void refilter(GtkEditable *_, void *view) {
  gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(gtk_tree_view_get_model(view))),
    select_first_item(view);
}

// Signal for a treeview keypress.
// This is needed to avoid triggering "row-activate" when pressing Enter, which collapses a
// multiple-selection.
static int list_keypress(GtkWidget *_, GdkEventKey *event, void *dialog) {
  if (event->keyval == GDK_KEY_Return) return (g_signal_emit_by_name(dialog, "response", 1), true);
  return false;
}

// Signal for an Enter keypress or double-click in the treeview.
static void row_activated(GtkTreeView *_, GtkTreePath *__, GtkTreeViewColumn *___, void *dialog) {
  g_signal_emit_by_name(dialog, "response", 1);
}

// Appends the selected row to the Lua table at the top of the Lua stack.
static void add_selected_row(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *_, void *__) {
  path = gtk_tree_model_filter_convert_path_to_child_path(GTK_TREE_MODEL_FILTER(model), path);
  int index = gtk_tree_path_get_indices(path)[0];
  lua_pushnumber(lua, index + 1), lua_rawseti(lua, -2, lua_rawlen(lua, -2) + 1);
}

int list_dialog(DialogOptions opts, lua_State *L) {
  int num_columns = opts.columns ? lua_rawlen(L, opts.columns) : 1,
      num_items = lua_rawlen(L, opts.items);
  GType cols[num_columns];
  for (int i = 0; i < num_columns; i++) cols[i] = G_TYPE_STRING;
  GtkListStore *store = gtk_list_store_newv(num_columns, cols);
  for (int i = 1, j = 0; i <= num_items; i++) {
    GtkTreeIter iter;
    if (j == 0) gtk_list_store_append(store, &iter);
    const char *item = (lua_rawgeti(L, opts.items, i), lua_tostring(L, -1));
    gtk_list_store_set(store, &iter, j++, item, -1), lua_pop(L, 1);
    if (j == num_columns) j = 0; // new row
  }
  GtkTreeModel *filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL);

  GtkWidget *dialog = new_dialog(&opts), *entry = gtk_entry_new(), *treeview;
  gtk_window_set_resizable(GTK_WINDOW(dialog), true);
  int window_width, window_height;
  get_size(&window_width, &window_height);
  gtk_window_resize(GTK_WINDOW(dialog), window_width - 200, 500);
  GtkDialog *dlg = GTK_DIALOG(dialog);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(dlg)), entry, false, true, 0);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(dlg)), scrolled, true, true, 0);
  gtk_container_add(GTK_CONTAINER(scrolled), treeview = gtk_tree_view_new_with_model(filter));
  gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter), visible, treeview, NULL);
  for (int i = 1; i <= num_columns; i++) {
    const char *header = opts.columns ? (lua_rawgeti(L, opts.columns, i), lua_tostring(L, -1)) : "";
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
      header, gtk_cell_renderer_text_new(), "text", i - 1, NULL);
    // gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), column);
    if (opts.columns) lua_pop(L, 1); // header
  }
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(treeview), opts.columns);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(treeview), true);
  gtk_tree_view_set_search_column(GTK_TREE_VIEW(treeview), opts.search_column - 1);
  gtk_tree_view_set_search_entry(GTK_TREE_VIEW(treeview), GTK_ENTRY(entry));
  gtk_tree_view_set_search_equal_func(GTK_TREE_VIEW(treeview), matches, NULL, NULL);
  g_signal_connect(entry, "changed", G_CALLBACK(refilter), treeview);
  g_signal_connect(treeview, "key-press-event", G_CALLBACK(list_keypress), dialog);
  g_signal_connect(treeview, "row-activated", G_CALLBACK(row_activated), dialog);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  if (opts.multiple) gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  // Set entry text here to initialize interactive search.
  if (opts.text) gtk_entry_set_text(GTK_ENTRY(entry), opts.text);
  select_first_item(GTK_TREE_VIEW(treeview));

  int button = (gtk_widget_show_all(dialog), gtk_dialog_run(dlg));
  bool cancelled = button < 1 || (button == 2 && !opts.return_button);
  if (cancelled || !gtk_tree_selection_count_selected_rows(selection))
    return (gtk_widget_destroy(dialog), 0);
  lua_newtable(L); // note: will be replaced by a single result if opts.multiple is false
  gtk_tree_selection_selected_foreach(selection, add_selected_row, NULL);
  if (!opts.multiple) lua_rawgeti(L, -1, 1), lua_replace(L, -2); // single result
  if (opts.return_button) lua_pushinteger(L, button);
  return (gtk_widget_destroy(dialog), !opts.return_button ? 1 : 2);
}

// Contains information about an active process.
struct Process {
#if !_WIN32
  int pid, fstdin;
#else
  HANDLE pid, fstdin;
#endif
  int fstdout, fstderr, exit_status;
  GIOChannel *cstdout, *cstderr;
};
static inline struct Process *PROCESS(struct Process *proc) { return proc; }

// Signal that channel output is available for reading.
static int read_channel(GIOChannel *source, GIOCondition cond, void *proc) {
  if (!PROCESS(proc)->pid || !(cond & G_IO_IN)) return false;
  char buf[BUFSIZ];
  size_t len = 0;
  do {
    int status = g_io_channel_read_chars(source, buf, BUFSIZ, &len, NULL);
    if (status == G_IO_STATUS_NORMAL && len > 0)
      process_output(PROCESS(proc), buf, len, source == PROCESS(proc)->cstdout);
  } while (len == BUFSIZ);
  return PROCESS(proc)->pid && !(cond & G_IO_HUP);
}

// Creates and returns a new channel for reading from the given file descriptor. The channel
// can optionally monitor that file descriptor for output.
static GIOChannel *new_channel(int fd, Process *proc, bool watch) {
#if !_WIN32
  GIOChannel *channel = g_io_channel_unix_new(fd);
#else
  GIOChannel *channel = g_io_channel_win32_new_fd(fd);
#endif
  g_io_channel_set_encoding(channel, NULL, NULL), g_io_channel_set_buffered(channel, false);
  if (watch)
    g_io_add_watch(channel, G_IO_IN | G_IO_HUP, read_channel, proc), g_io_channel_unref(channel);
  return channel;
}

// Cleans up after the process finished executing and returned the given status code.
static void cleanup_process(struct Process *proc, int status) {
  g_source_remove_by_user_data(proc); // disconnect stdout watch
  g_source_remove_by_user_data(proc); // disconnect stderr watch
  g_source_remove_by_user_data(proc); // disconnect child watch
  g_spawn_close_pid(proc->pid), proc->pid = 0;
#if !_WIN32
  close(proc->fstdin), close(proc->fstdout), close(proc->fstderr);
#else
  CloseHandle(proc->fstdin), _close(proc->fstdout), _close(proc->fstderr);
#endif
  process_exited(proc, proc->exit_status = status);
}

// Signal that the child process finished.
static void proc_exited(GPid _, int status, void *proc) { cleanup_process(proc, status); }

bool spawn(lua_State *L, Process *proc_, int index, const char *cmd, const char *cwd, int envi,
  bool monitor_stdout, bool monitor_stderr, const char **error) {
  struct Process *proc = proc_;
#if !_WIN32
  // Construct argv from cmd and envp from envi.
  int envc = envi ? lua_rawlen(L, envi) : 0;
  char **argv, *envp[envc + 1];
  GError *err = NULL;
  if (!g_shell_parse_argv(cmd, NULL, &argv, &err)) return (*error = err->message, false);
  if (lua_checkstack(L, envc), envi)
    for (int i = (lua_pushnil(L), 0); lua_next(L, envi); lua_pop(L, 1), i++)
      envp[i] = (char *)(lua_pushvalue(L, -1), lua_insert(L, -3), lua_tostring(L, -3));
  envp[envc] = NULL;
  // Spawn the process with pipes for stdin, stdout, and stderr.
  bool ok = g_spawn_async_with_pipes(cwd, argv, envi ? envp : NULL,
    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, NULL, NULL, &proc->pid, &proc->fstdin,
    &proc->fstdout, &proc->fstderr, &err);
  if (g_strfreev(argv), !ok) return (*error = err->message, false);
#else
  // Reconstruct cmd and construct envp from envi.
  // Use "cmd.exe /c" for more versatility (e.g. spawning batch files).
  // envp needs to be a contiguous block of 'key=value\0' strings terminated by another '\0'.
  cmd = (lua_pushstring(L, getenv("COMSPEC")), lua_pushliteral(L, " /c "), lua_pushstring(L, cmd),
    lua_concat(L, 3), lua_tostring(L, -1));
  char *envp = NULL;
  if (envi) {
    luaL_Buffer buf;
    for (luaL_buffinit(L, &buf), lua_pushnil(L); lua_next(L, envi); lua_pop(L, 1))
      luaL_addstring(&buf, lua_tostring(L, -1)), luaL_addchar(&buf, '\0');
    luaL_addchar(&buf, '\0'), luaL_pushresult(&buf);
    envp = memcpy(malloc(lua_rawlen(L, -1)), lua_tostring(L, -1), lua_rawlen(L, -1));
  }
  // Setup pipes for stdin, stdout, and stderr. (Adapted from SciTE.)
  SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, true};
  HANDLE stdin_read, proc_stdout, stdout_write, proc_stderr, stderr_write;
  // Redirect stdin.
  CreatePipe(&stdin_read, &proc->fstdin, &sa, 0);
  SetHandleInformation(proc->fstdin, HANDLE_FLAG_INHERIT, 0);
  // Redirect stdout.
  CreatePipe(&proc_stdout, &stdout_write, &sa, 0);
  SetHandleInformation(proc_stdout, HANDLE_FLAG_INHERIT, 0);
  // Redirect stderr.
  CreatePipe(&proc_stderr, &stderr_write, &sa, 0);
  SetHandleInformation(proc_stderr, HANDLE_FLAG_INHERIT, 0);
  // Spawn the process with pipes and no window.
  STARTUPINFOA startup_info = {sizeof(STARTUPINFOA), NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0,
    STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES, SW_HIDE, 0, NULL, stdin_read, stdout_write,
    stderr_write};
  PROCESS_INFORMATION proc_info = {NULL, NULL, 0, 0};
  bool ok = CreateProcessA(NULL, (LPSTR)cmd, NULL, NULL, true, CREATE_NEW_PROCESS_GROUP, envp,
    cwd ? cwd : NULL, &startup_info, &proc_info);
  if (envp) free(envp);
  if (!ok) {
    static char err[65535];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), 0, (LPSTR)err, 65535, NULL);
    return (*error = err, false);
  }
  proc->pid = proc_info.hProcess;
  proc->fstdout = _open_osfhandle((intptr_t)proc_stdout, _O_RDONLY); // transfers ownership
  proc->fstderr = _open_osfhandle((intptr_t)proc_stderr, _O_RDONLY); // transfers ownership
  // Close unneeded handles.
  CloseHandle(proc_info.hThread);
  CloseHandle(stdin_read), CloseHandle(stdout_write), CloseHandle(stderr_write);
#endif
  // Monitor stdout, stderr, and the process itself.
  proc->cstdout = new_channel(proc->fstdout, proc, monitor_stdout),
  proc->cstderr = new_channel(proc->fstderr, proc, monitor_stderr);
  return (g_child_watch_add(proc->pid, proc_exited, proc), true);
}

size_t process_size() { return sizeof(struct Process); }

bool is_process_running(Process *proc) { return PROCESS(proc)->pid; }

void wait_process(Process *proc) {
#if !_WIN32
  int status;
  waitpid(PROCESS(proc)->pid, &status, 0), status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#else
  DWORD status;
  WaitForSingleObject(PROCESS(proc)->pid, INFINITE),
    GetExitCodeProcess(PROCESS(proc)->pid, &status);
#endif
  cleanup_process(proc, status);
}

char *read_process_output(Process *proc, char option, size_t *len, const char **error, int *code) {
  char *buf;
  GError *err = NULL;
  GIOStatus status = G_IO_STATUS_NORMAL;
  if (!g_io_channel_get_buffered(PROCESS(proc)->cstdout))
    g_io_channel_set_buffered(PROCESS(proc)->cstdout, true); // needed for manual read functions
  if (option == 'l' || option == 'L') {
    GString *s = g_string_new(NULL);
    status = g_io_channel_read_line_string(PROCESS(proc)->cstdout, s, NULL, &err);
    *len = s->len, buf = g_string_free(s, false);
  } else if (option == 'a') {
    status = g_io_channel_read_to_end(PROCESS(proc)->cstdout, &buf, len, &err);
    if (status == G_IO_STATUS_EOF) status = G_IO_STATUS_NORMAL;
  } else if (option == 'n') {
    size_t bytes = *len;
    status = g_io_channel_read_chars(PROCESS(proc)->cstdout, buf = malloc(bytes), bytes, len, &err);
  }
  if ((g_io_channel_get_buffer_condition(PROCESS(proc)->cstdout) & G_IO_IN) == 0)
    g_io_channel_set_buffered(PROCESS(proc)->cstdout, false); // needed for stdout callback
  if (option == 'l' && buf[*len - 1] == '\n') (*len)--;
  if (option == 'l' && buf[*len - 1] == '\r') (*len)--;
  if (status == G_IO_STATUS_EOF) return (free(buf), *error = NULL, NULL);
  if (status != G_IO_STATUS_NORMAL) *error = err->message, *code = err->code, free(buf), buf = NULL;
  return buf;
}

void write_process_input(Process *proc, const char *s, size_t len) {
#if !_WIN32
  write(PROCESS(proc)->fstdin, s, len);
#else
  DWORD len_written;
  WriteFile(PROCESS(proc)->fstdin, s, len, &len_written, NULL);
#endif
}

void close_process_input(Process *proc) {
#if !_WIN32
  close(PROCESS(proc)->fstdin);
#else
  CloseHandle(PROCESS(proc)->fstdin);
#endif
}

void kill_process(Process *proc, int signal) {
#if !_WIN32
  kill(PROCESS(proc)->pid, signal ? signal : SIGKILL);
#else
  TerminateProcess(PROCESS(proc)->pid, 1);
#endif
}

int get_process_exit_status(Process *proc) { return PROCESS(proc)->exit_status; }

void quit() {
  GdkEventAny event = {GDK_DELETE, gtk_widget_get_window(window), true};
  gdk_event_put((GdkEvent *)&event);
}

// Signal for processing a remote Textadept's command line arguments.
static int process(GApplication *_, GApplicationCommandLine *line, void *buf) {
  if (!lua) return 0; // only process argv for secondary/remote instances
#if !_WIN32
  int argc = 0;
  char **argv = g_application_command_line_get_arguments(line, &argc);
  const char *cwd = g_application_command_line_get_cwd(line);
#else
  char **argv = g_strsplit(buf, "\n", 0), *cwd = argv[0];
  int argc = g_strv_length(argv);
#endif
  if (argc > 1) {
    lua_newtable(lua);
    lua_pushstring(lua, cwd), lua_rawseti(lua, -2, -1);
    while (--argc) lua_pushstring(lua, argv[argc]), lua_rawseti(lua, -2, argc);
    emit("command_line", LUA_TTABLE, luaL_ref(lua, LUA_REGISTRYINDEX), -1);
  }
  g_strfreev(argv);
  return (gtk_window_present(GTK_WINDOW(window)), 0);
}

#if _WIN32
// Replacement for `g_application_register()` that always "works".
int g_application_register(GApplication *_, GCancellable *__, GError **___) { return true; }

// Replacement for `g_application_get_is_remote()` that uses a named pipe.
int g_application_get_is_remote(GApplication *_) {
  return WaitNamedPipe(pipe_id, NMPWAIT_WAIT_FOREVER) != 0;
}

// Replacement for `g_application_run()` that handles multiple instances.
int g_application_run(GApplication *_, int __, char **___) {
  HANDLE pipe = CreateFile(pipe_id, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  char cwd[FILENAME_MAX + 1]; // TODO: is this big enough?
  GetCurrentDirectory(FILENAME_MAX + 1, cwd);
  DWORD len_written;
  WriteFile(pipe, cwd, strlen(cwd) + 1, &len_written, NULL);
  for (int i = 1; i < __argc; i++)
    WriteFile(pipe, __argv[i], strlen(__argv[i]) + 1, &len_written, NULL);
  return (CloseHandle(pipe), 0);
}

// Processes a remote Textadept's command line arguments.
static int pipe_read(void *buf) { return (process(NULL, NULL, buf), free(buf), false); }

// Listens for remote Textadept communications and reads command line arguments.
// Processing can only happen in the GTK main thread because GTK is single-threaded.
static DWORD WINAPI pipe_listener(HANDLE pipe) {
  while (true)
    if (pipe != INVALID_HANDLE_VALUE && ConnectNamedPipe(pipe, NULL)) {
      char *buf = malloc(65536 * sizeof(char)), *p = buf; // arbitrary size
      DWORD len;
      while (ReadFile(pipe, p, buf + 65536 - 1 - p, &len, NULL) && len > 0) p += len;
      for (*p = '\0', len = p - buf - 1, p = buf; p < buf + len; p++)
        if (!*p) *p = '\n'; // but preserve trailing '\0'
      g_idle_add(pipe_read, buf), DisconnectNamedPipe(pipe);
    }
  return 0;
}
#endif

// Runs Textadept.
// On Windows, also creates a pipe and thread for communication with remote instances.
int main(int argc, char **argv) {
  gtk_init(&argc, &argv);

  bool force = false;
  for (int i = 0; i < argc; i++)
    if (strcmp("-f", argv[i]) == 0 || strcmp("--force", argv[i]) == 0) {
      force = true;
      break;
    }
  GApplication *app = g_application_new("textadept.editor", G_APPLICATION_HANDLES_COMMAND_LINE);
  g_signal_connect(app, "command-line", G_CALLBACK(process), NULL);
  if (g_application_register(app, NULL, NULL) && g_application_get_is_remote(app) && !force)
    return (g_application_run(app, argc, argv), g_object_unref(app), 0);

#if __APPLE__
  osxapp = g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
#endif
  if (!init_textadept(argc, argv)) return (g_object_unref(app), 1);
#if _WIN32
  HANDLE pipe = NULL, thread = NULL;
  if (!g_application_get_is_remote(app))
    pipe = CreateNamedPipe(pipe_id, PIPE_ACCESS_INBOUND, PIPE_WAIT, 1, 0, 0, INFINITE, NULL),
    thread = CreateThread(NULL, 0, &pipe_listener, pipe, 0, NULL);
  gtk_main();
  if (pipe && thread) TerminateThread(thread, 0), CloseHandle(thread), CloseHandle(pipe);
#elif __APPLE__
  gtkosx_application_ready(osxapp), gtk_main();
#else
  gtk_main();
#endif

  return (g_object_unref(app), 0); // close_textadept() was called before gtk_main_quit()
}

#if _WIN32
// Runs Textadept in Windows.
// Note: __argc and __argv are MSVC extensions.
int WINAPI WinMain(HINSTANCE _, HINSTANCE __, LPSTR ___, int ____) { return main(__argc, __argv); }
#endif
