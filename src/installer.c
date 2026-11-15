#define _POSIX_C_SOURCE 200809L
#include <adwaita.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "bundle.h"

typedef struct {
    char *installed;
    gboolean already;
} CheckResult;

typedef struct {
    AdwApplication *app;
    GtkWindow *window;
    AdwStatusPage *status;
    GtkButton *button;
    GtkProgressBar *progress;
    GtkTextBuffer *details;
    GtkExpander *expander;
    char *package;
    char *screenshot;
    gboolean checking;
    gboolean installing;
    gboolean ready;
    gboolean complete;
    int exit_code;
    guint pulse;
} Installer;

static GQuark installer_error(void) {
    return g_quark_from_static_string("codex-cachyos-installer");
}

static void check_result_free(gpointer data) {
    CheckResult *r = data;
    g_free(r->installed);
    g_free(r);
}

static char *program_output(const char *const *argv, GError **error) {
    g_autoptr(GSubprocess) p = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    if (!p) return NULL;
    char *out = NULL;
    g_autofree char *err = NULL;
    if (!g_subprocess_communicate_utf8(p, NULL, NULL, &out, &err, error)) {
        g_free(out);
        return NULL;
    }
    if (!g_subprocess_get_successful(p)) {
        g_set_error(error, installer_error(), 1, "%s", err && *err ? err : "Команда не выполнена.");
        g_free(out);
        return NULL;
    }
    return out;
}

static void verify_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source; (void)cancel;
    Installer *s = task_data;
    struct utsname machine;
    if (uname(&machine) != 0 || g_strcmp0(machine.machine, "x86_64")) {
        g_task_return_new_error(task, installer_error(), 1, "Нужен компьютер с архитектурой x86_64 (Intel или AMD).");
        return;
    }
    if (!g_file_test("/etc/arch-release", G_FILE_TEST_EXISTS)) {
        g_task_return_new_error(task, installer_error(), 1, "Этот установщик предназначен для CachyOS и Arch Linux.");
        return;
    }
    const char *needed[] = {"/usr/bin/pacman", "/usr/bin/pkexec", "/usr/bin/vercmp", NULL};
    for (int i = 0; needed[i]; i++) {
        if (!g_file_test(needed[i], G_FILE_TEST_IS_EXECUTABLE)) {
            g_task_return_new_error(task, installer_error(), 1, "Не найден системный компонент: %s", needed[i]);
            return;
        }
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = g_file_new_for_path(s->package);
    g_autoptr(GFileInputStream) input = g_file_read(file, NULL, &error);
    if (!input) {
        g_task_return_new_error(task, installer_error(), 1,
            "Не удалось открыть пакет. Распакуйте всю папку из ZIP.\n%s", error->message);
        return;
    }
    g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);
    unsigned char block[262144];
    gssize n;
    while ((n = g_input_stream_read(G_INPUT_STREAM(input), block, sizeof block, NULL, &error)) > 0)
        g_checksum_update(hash, block, n);
    if (n < 0) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    if (g_strcmp0(g_checksum_get_string(hash), PACKAGE_SHA256)) {
        g_task_return_new_error(task, installer_error(), 1,
            "Контрольная сумма пакета не совпадает. Скачайте и распакуйте архив заново.");
        return;
    }
    const char *query[] = {"/usr/bin/pacman", "-Qp", s->package, NULL};
    g_autofree char *description = program_output(query, &error);
    if (!description || g_strcmp0(g_strstrip(description), "chatgpt " APP_VERSION "-1")) {
        g_task_return_new_error(task, installer_error(), 1, "Не удалось проверить метаданные пакета: %s",
            error ? error->message : "неожиданное имя или версия");
        return;
    }
    CheckResult *result = g_new0(CheckResult, 1);
    const char *installed[] = {"/usr/bin/pacman", "-Q", "chatgpt", NULL};
    g_clear_error(&error);
    g_autofree char *current = program_output(installed, &error);
    if (current) {
        g_strstrip(current);
        if (g_str_has_prefix(current, "chatgpt ")) {
            result->installed = g_strdup(current + 8);
            const char *cmp[] = {"/usr/bin/vercmp", result->installed, APP_VERSION "-1", NULL};
            g_clear_error(&error);
            g_autofree char *comparison = program_output(cmp, &error);
            if (!comparison) {
                check_result_free(result);
                g_task_return_error(task, g_steal_pointer(&error));
                return;
            }
            result->already = g_ascii_strtoll(comparison, NULL, 10) >= 0;
        }
    }
    if (!result->already) {
        struct statvfs space;
        if (statvfs("/", &space) != 0 || (guint64)space.f_bavail * space.f_frsize < 3ULL * 1024 * 1024 * 1024) {
            check_result_free(result);
            g_task_return_new_error(task, installer_error(), 1, "Для установки нужно не менее 3 ГБ свободного места в системном разделе.");
            return;
        }
        if (g_file_test("/var/lib/pacman/db.lck", G_FILE_TEST_EXISTS)) {
            check_result_free(result);
            g_task_return_new_error(task, installer_error(), 1,
                "Менеджер пакетов занят. Дождитесь завершения другой установки и повторите проверку.");
            return;
        }
    }
    g_task_return_pointer(task, result, check_result_free);
}

static gboolean pulse_progress(gpointer data) {
    Installer *s = data;
    gtk_progress_bar_pulse(s->progress);
    return G_SOURCE_CONTINUE;
}

static void set_busy(Installer *s, gboolean busy) {
    gtk_widget_set_visible(GTK_WIDGET(s->progress), busy);
    gtk_widget_set_sensitive(GTK_WIDGET(s->button), !busy);
    gtk_window_set_deletable(s->window, !s->installing);
    if (busy && !s->pulse) s->pulse = g_timeout_add(120, pulse_progress, s);
    if (!busy && s->pulse) { g_source_remove(s->pulse); s->pulse = 0; }
}

static void set_error(Installer *s, const char *message) {
    s->exit_code = 1;
    s->ready = FALSE;
    s->complete = FALSE;
    adw_status_page_set_icon_name(s->status, "dialog-error-symbolic");
    adw_status_page_set_title(s->status, "Не удалось продолжить");
    adw_status_page_set_description(s->status, message);
    gtk_button_set_label(s->button, "Повторить проверку");
    set_busy(s, FALSE);
}

static void verification_done(GObject *object, GAsyncResult *async, gpointer data) {
    (void)object;
    Installer *s = data;
    s->checking = FALSE;
    g_autoptr(GError) error = NULL;
    CheckResult *r = g_task_propagate_pointer(G_TASK(async), &error);
    if (!r) { set_error(s, error->message); return; }
    s->ready = TRUE;
    s->complete = r->already;
    s->exit_code = 0;
    adw_status_page_set_icon_name(s->status, r->already ? "emblem-ok-symbolic" : "system-software-install-symbolic");
    if (r->already) {
        adw_status_page_set_title(s->status, "ChatGPT уже установлен");
        g_autofree char *text = g_strdup_printf("Установлена версия %s. Понижать её не требуется.", r->installed);
        adw_status_page_set_description(s->status, text);
        gtk_button_set_label(s->button, "Открыть ChatGPT");
    } else {
        adw_status_page_set_title(s->status, "ChatGPT и Codex");
        g_autofree char *text = r->installed
            ? g_strdup_printf("Обновление %s → " APP_VERSION ". Приложение появится в меню GNOME.", r->installed)
            : g_strdup("Документы, разговоры и работа с кодом. Приложение появится в меню GNOME.");
        adw_status_page_set_description(s->status, text);
        gtk_button_set_label(s->button, r->installed ? "Обновить" : "Установить");
    }
    check_result_free(r);
    set_busy(s, FALSE);
}

static void begin_verification(Installer *s) {
    s->checking = TRUE;
    s->ready = FALSE;
    adw_status_page_set_icon_name(s->status, "system-software-install-symbolic");
    adw_status_page_set_title(s->status, "Проверяю установочный пакет");
    adw_status_page_set_description(s->status, "Это займёт несколько секунд.");
    set_busy(s, TRUE);
    GTask *task = g_task_new(NULL, NULL, verification_done, s);
    g_task_set_task_data(task, s, NULL);
    g_task_run_in_thread(task, verify_worker);
    g_object_unref(task);
}

static void installation_done(GObject *object, GAsyncResult *result, gpointer data) {
    Installer *s = data;
    GSubprocess *p = G_SUBPROCESS(object);
    char *out = NULL, *err = NULL;
    g_autoptr(GError) error = NULL;
    gboolean communicated = g_subprocess_communicate_utf8_finish(p, result, &out, &err, &error);
    s->installing = FALSE;
    g_autofree char *log = g_strdup_printf("ChatGPT / Codex " APP_VERSION "\n\n%s\n%s\n%s",
        out ? out : "", err ? err : "", error ? error->message : "");
    g_free(out); g_free(err);
    gtk_text_buffer_set_text(s->details, log, -1);
    g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "codex-cachyos-installer", NULL);
    if (g_mkdir_with_parents(dir, 0700) == 0) {
        g_autofree char *path = g_build_filename(dir, "install.log", NULL);
        g_file_set_contents(path, log, -1, NULL);
    }
    if (!communicated || !g_subprocess_get_successful(p)) {
        int code = communicated && g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
        if (code == 126 || code == 127)
            set_error(s, "Авторизация отменена или права администратора не получены. Можно повторить установку.");
        else
            set_error(s, "pacman не завершил установку. Откройте подробности ниже. При устаревших зеркалах обновите систему штатным инструментом CachyOS и повторите.");
        gtk_expander_set_expanded(s->expander, TRUE);
        g_object_unref(p);
        return;
    }
    const char *query[] = {"/usr/bin/pacman", "-Q", "chatgpt", NULL};
    g_clear_error(&error);
    g_autofree char *version = program_output(query, &error);
    if (!version || g_strcmp0(g_strstrip(version), "chatgpt " APP_VERSION "-1")) {
        set_error(s, "Команда завершилась, но ожидаемая версия не найдена. Проверьте подробности установки.");
        g_object_unref(p);
        return;
    }
    s->complete = TRUE;
    s->ready = TRUE;
    s->exit_code = 0;
    adw_status_page_set_icon_name(s->status, "emblem-ok-symbolic");
    adw_status_page_set_title(s->status, "Готово");
    adw_status_page_set_description(s->status, "ChatGPT установлен. Войдите в свой аккаунт на этом компьютере. Если приложение уже было открыто, перезапустите его.");
    gtk_button_set_label(s->button, "Открыть ChatGPT");
    set_busy(s, FALSE);
    g_object_unref(p);
}

static void primary_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Installer *s = data;
    if (s->checking || s->installing) return;
    if (s->complete) {
        g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new("chatgpt.desktop");
        g_autoptr(GError) error = NULL;
        g_autoptr(GdkAppLaunchContext) context = gdk_display_get_app_launch_context(gtk_widget_get_display(GTK_WIDGET(s->window)));
        if (!info || !g_app_info_launch(G_APP_INFO(info), NULL, G_APP_LAUNCH_CONTEXT(context), &error)) {
            adw_status_page_set_description(s->status, error ? error->message : "Откройте ChatGPT из меню приложений.");
            return;
        }
        g_application_quit(G_APPLICATION(s->app));
        return;
    }
    if (!s->ready) { begin_verification(s); return; }
    if (g_file_test("/var/lib/pacman/db.lck", G_FILE_TEST_EXISTS)) {
        set_error(s, "Менеджер пакетов занят. Дождитесь завершения другой установки и повторите проверку.");
        return;
    }
    const char *argv[] = {"/usr/bin/pkexec", "/usr/bin/pacman", "-U", "--needed", "--noconfirm", "--", s->package, NULL};
    g_autoptr(GError) error = NULL;
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    if (!p) { set_error(s, error->message); return; }
    s->installing = TRUE;
    adw_status_page_set_title(s->status, "Устанавливаю ChatGPT");
    adw_status_page_set_description(s->status, "Подтвердите установку в системном окне. Если нужны зависимости, pacman загрузит их из репозиториев.");
    set_busy(s, TRUE);
    g_subprocess_communicate_utf8_async(p, NULL, NULL, installation_done, s);
}

static gboolean close_requested(GtkWindow *window, gpointer data) {
    (void)window;
    Installer *s = data;
    return s->checking || s->installing;
}

static gboolean capture_window(gpointer data) {
    Installer *s = data;
    if (s->checking) return G_SOURCE_CONTINUE;
    GtkWidget *widget = GTK_WIDGET(s->window);
    g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
    if (node) {
        GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(s->window));
        graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, width, height);
        g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &viewport);
        if (!gdk_texture_save_to_png(texture, s->screenshot)) s->exit_code = 1;
    } else s->exit_code = 1;
    g_application_quit(G_APPLICATION(s->app));
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *app, gpointer data) {
    Installer *s = data;
    if (s->window) { gtk_window_present(s->window); return; }
    s->window = GTK_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(s->window, "Установка ChatGPT / Codex");
    gtk_window_set_default_size(s->window, 600, 680);
    gtk_window_set_resizable(s->window, FALSE);
    g_signal_connect(s->window, "close-request", G_CALLBACK(close_requested), s);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(s->window), outer);
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new("ChatGPT / Codex", "Установка для CachyOS"));
    gtk_box_append(GTK_BOX(outer), header);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(content, 32); gtk_widget_set_margin_end(content, 32);
    gtk_widget_set_margin_top(content, 12); gtk_widget_set_margin_bottom(content, 24);
    gtk_box_append(GTK_BOX(outer), content);
    s->status = ADW_STATUS_PAGE(adw_status_page_new());
    gtk_widget_set_size_request(GTK_WIDGET(s->status), -1, 340);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(s->status));
    GtkWidget *version = gtk_label_new("Версия " APP_VERSION "  ·  x86_64");
    gtk_widget_add_css_class(version, "dim-label");
    gtk_box_append(GTK_BOX(content), version);
    GtkWidget *note = gtk_label_new("Приложение включено в архив.\nДля системных зависимостей может понадобиться интернет.");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(note, "dim-label");
    gtk_box_append(GTK_BOX(content), note);
    s->progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(s->progress));
    s->button = GTK_BUTTON(gtk_button_new_with_label("Установить"));
    gtk_widget_add_css_class(GTK_WIDGET(s->button), "suggested-action");
    gtk_widget_add_css_class(GTK_WIDGET(s->button), "pill");
    gtk_widget_set_halign(GTK_WIDGET(s->button), GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(GTK_WIDGET(s->button), 200, 44);
    g_signal_connect(s->button, "clicked", G_CALLBACK(primary_clicked), s);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(s->button));
    s->expander = GTK_EXPANDER(gtk_expander_new("Подробности"));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 140);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
    GtkWidget *text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
    s->details = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    gtk_text_buffer_set_text(s->details, "Здесь появится журнал установки pacman.", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
    gtk_expander_set_child(s->expander, scroll);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(s->expander));
    gtk_window_present(s->window);
    begin_verification(s);
    if (s->screenshot) g_timeout_add(2000, capture_window, s);
}

static gboolean done_console;
static int console_status;
static void console_check_done(GObject *object, GAsyncResult *result, gpointer data) {
    (void)object; (void)data;
    g_autoptr(GError) error = NULL;
    CheckResult *r = g_task_propagate_pointer(G_TASK(result), &error);
    if (!r) { g_printerr("ERROR: %s\n", error->message); console_status = 1; }
    else {
        g_print("OK: chatgpt " APP_VERSION "-1 x86_64; SHA-256 verified; installed=%s; already=%s\n",
            r->installed ? r->installed : "none", r->already ? "yes" : "no");
        check_result_free(r);
    }
    done_console = TRUE;
}

int main(int argc, char **argv) {
    Installer s = {0};
    g_autoptr(GError) error = NULL;
    g_autofree char *exe = g_file_read_link("/proc/self/exe", &error);
    if (!exe) { g_printerr("%s\n", error->message); return 1; }
    g_autofree char *dir = g_path_get_dirname(exe);
    s.package = g_build_filename(dir, PACKAGE_FILE, NULL);
    if (argc == 2 && !g_strcmp0(argv[1], "--check")) {
        GTask *task = g_task_new(NULL, NULL, console_check_done, NULL);
        g_task_set_task_data(task, &s, NULL);
        g_task_run_in_thread(task, verify_worker);
        g_object_unref(task);
        while (!done_console) g_main_context_iteration(NULL, TRUE);
        g_free(s.package);
        return console_status;
    }
    if (argc == 3 && !g_strcmp0(argv[1], "--screenshot")) s.screenshot = g_strdup(argv[2]);
    else if (argc != 1) { g_printerr("Usage: Установить [--check | --screenshot FILE]\n"); g_free(s.package); return 2; }
    if (geteuid() == 0) { g_printerr("Откройте установщик от обычного пользователя. Пароль запросит система.\n"); g_free(s.package); g_free(s.screenshot); return 1; }
    s.app = adw_application_new("io.github.edgarsskills.CodexCachyOSInstaller", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(s.app, "activate", G_CALLBACK(activate), &s);
    int code = g_application_run(G_APPLICATION(s.app), 1, argv);
    if (s.pulse) g_source_remove(s.pulse);
    g_object_unref(s.app);
    g_free(s.package); g_free(s.screenshot);
    return code ? code : s.exit_code;
}
