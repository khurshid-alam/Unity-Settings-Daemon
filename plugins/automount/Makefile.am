libexec_PROGRAMS = unity-fallback-mount-helper

unity_fallback_mount_helper_SOURCES = \
	gnome-fallback-mount-helper.c \
	gsd-automount-manager.c \
	gsd-automount-manager.h \
	gsd-autorun.c \
	gsd-autorun.h

unity_fallback_mount_helper_CPPFLAGS = \
	-I$(top_srcdir)/gnome-settings-daemon		\
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\" \
	$(AM_CPPFLAGS)

unity_fallback_mount_helper_CFLAGS =	\
	$(SETTINGS_PLUGIN_CFLAGS)	\
	$(AUTOMOUNT_CFLAGS)

unity_fallback_mount_helper_LDADD =	\
	$(SETTINGS_PLUGIN_LIBS)		\
	$(AUTOMOUNT_LIBS)		\
	$(top_builddir)/gnome-settings-daemon/libgsd.la

autostartdir = $(sysconfdir)/xdg/autostart
autostart_in_files = unity-fallback-mount-helper.desktop.in
autostart_in_in_files = unity-fallback-mount-helper.desktop.in.in
autostart_DATA = $(autostart_in_files:.desktop.in=.desktop)

$(autostart_in_files): $(autostart_in_in_files)
	@sed -e "s|\@LIBEXECDIR\@|$(libexecdir)|" $< > $@

@INTLTOOL_DESKTOP_RULE@

EXTRA_DIST = $(autostart_in_in_files)

CLEANFILES = $(autostart_DATA) $(autostart_in_files)
