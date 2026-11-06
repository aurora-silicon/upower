/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "config.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "linux/up-kbd-backlight-led.h"
#include "up-native.h"

static GIOCondition poll_condition;

typedef struct {
	const gchar *path;
	gboolean has_watch;
	gint condition; /* -1 skips dispatch before finalization. */
	gboolean watch_remains;
} WatchTest;

/**
 * poll_watch:
 *
 * Simulate sysfs priority and terminal events without hardware.
 **/
static gint
poll_watch (GPollFD *fds, guint n_fds, gint timeout)
{
	gint ready = 0;

	for (guint i = 0; i < n_fds; i++) {
		fds[i].revents = (fds[i].events & G_IO_PRI) ? poll_condition : 0;
		ready += fds[i].revents != 0;
	}

	return ready;
}

/**
 * up_native_get_native_path:
 * @object: the test's native tracking object
 *
 * Supply the fixture's filesystem path without requiring a udev device or
 * restricting the temporary directory to valid D-Bus object path characters.
 **/
const gchar *
up_native_get_native_path (GObject *object)
{
	return g_object_get_data (object, "native-path");
}

/**
 * get_poll_count:
 *
 * Count registered poll descriptors, even when none is ready.
 **/
static gint
get_poll_count (GMainContext *context)
{
	gint priority;
	gint timeout;
	gint count;

	g_assert_true (g_main_context_acquire (context));
	g_main_context_prepare (context, &priority);
	count = g_main_context_query (context, priority, &timeout, NULL, 0);
	g_main_context_release (context);

	return count;
}

/**
 * write_attribute:
 *
 * Create a simulated LED attribute in a temporary directory.
 **/
static void
write_attribute (const gchar *directory, const gchar *name, const gchar *value)
{
	g_autofree gchar *path = g_build_filename (directory, name, NULL);
	g_autoptr (GError) error = NULL;

	g_assert_true (g_file_set_contents (path, value, -1, &error));
	g_assert_no_error (error);
}

/**
 * remove_attribute:
 *
 * Remove a simulated LED attribute after the device has been destroyed.
 **/
static void
remove_attribute (const gchar *directory, const gchar *name)
{
	g_autofree gchar *path = g_build_filename (directory, name, NULL);

	g_assert_cmpint (g_remove (path), ==, 0);
}

/**
 * up_test_kbd_backlight_watch_cleanup:
 *
 * Exercise the LED coldplug and finalize methods without exporting D-Bus.
 **/
static void
up_test_kbd_backlight_watch_cleanup (gconstpointer data)
{
	const WatchTest *test = data;
	GMainContext *context = g_main_context_default ();
	g_autoptr (GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp ("upower_kbd_backlight_XXXXXX", &error);
	g_autoptr (GObject) native = NULL;
	g_autoptr (UpDeviceKbdBacklight) device = NULL;
	gpointer weak_device;
	gint original_poll_count;

	g_assert_no_error (error);
	write_attribute (directory, "max_brightness", "100");
	write_attribute (directory, "brightness", "50");
	if (test->has_watch)
		write_attribute (directory, "brightness_hw_changed", "75");

	native = g_object_new (G_TYPE_OBJECT, NULL);
	g_object_set_data_full (native, "native-path", g_strdup (directory), g_free);
	device = g_object_new (UP_TYPE_KBD_BACKLIGHT_LED, "native", native, NULL);
	original_poll_count = get_poll_count (context);
	g_assert_true (UP_DEVICE_KBD_BACKLIGHT_GET_CLASS (device)->coldplug (device));
	g_assert_cmpint (get_poll_count (context), ==, original_poll_count + test->has_watch);

	if (test->condition >= 0) {
		GSource *source = g_main_context_find_source_by_user_data (context, device);
		GPollFunc original_poll = g_main_context_get_poll_func (context);
		gboolean dispatched;
		gint brightness;

		g_assert_nonnull (source);
		poll_condition = test->condition;
		g_main_context_set_poll_func (context, poll_watch);
		if (poll_condition == 0)
			g_source_set_ready_time (source, 0);
		dispatched = g_main_context_iteration (context, FALSE);
		g_main_context_set_poll_func (context, original_poll);
		g_assert_true (dispatched);
		g_assert_cmpint (get_poll_count (context), ==,
				 original_poll_count + test->watch_remains);
		g_object_get (device, "brightness", &brightness, NULL);
		g_assert_cmpint (brightness, ==, test->watch_remains ? 75 : 50);
	}

	weak_device = device;
	g_object_add_weak_pointer (G_OBJECT (device), &weak_device);
	g_clear_object (&device);
	g_assert_null (weak_device);
	/* A closed fd left in the poll set would keep waking the main loop. */
	g_assert_cmpint (get_poll_count (context), ==, original_poll_count);

	if (test->has_watch)
		remove_attribute (directory, "brightness_hw_changed");
	remove_attribute (directory, "brightness");
	remove_attribute (directory, "max_brightness");
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
	static const WatchTest tests[] = {
		{ "/kbd-backlight/finalize-with-watch", TRUE, -1, TRUE },
		{ "/kbd-backlight/finalize-without-watch", FALSE, -1, FALSE },
		{ "/kbd-backlight/finalize-after-watch-removed", TRUE, 0, FALSE },
		{ "/kbd-backlight/hangup", TRUE, G_IO_HUP, FALSE },
		{ "/kbd-backlight/invalid-fd", TRUE, G_IO_NVAL, FALSE },
		{ "/kbd-backlight/hangup-with-priority", TRUE, G_IO_HUP | G_IO_PRI, FALSE },
		{ "/kbd-backlight/invalid-fd-with-priority", TRUE, G_IO_NVAL | G_IO_PRI, FALSE },
		{ "/kbd-backlight/priority", TRUE, G_IO_PRI, TRUE },
		{ "/kbd-backlight/priority-with-error", TRUE, G_IO_PRI | G_IO_ERR, TRUE },
	};

	g_test_init (&argc, &argv, NULL);
	for (guint i = 0; i < G_N_ELEMENTS (tests); i++)
		g_test_add_data_func (tests[i].path, &tests[i], up_test_kbd_backlight_watch_cleanup);

	return g_test_run ();
}
