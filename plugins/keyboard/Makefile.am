NULL =

plugin_name = keyboard

plugin_LTLIBRARIES = \
	libkeyboard.la		\
	$(NULL)

themedir = $(pkgdatadir)/icons/hicolor
size = 64x64
context = devices

iconsdir = $(themedir)/$(size)/$(context)
icons_DATA = \
	  kbd-capslock-off.png kbd-numlock-off.png kbd-scrolllock-off.png \
	  kbd-capslock-on.png kbd-numlock-on.png kbd-scrolllock-on.png

FCITX_SOURCES =
BUILT_SOURCES =

if HAVE_FCITX
FCITX_SOURCES += 	\
	fcitx.c		\
	fcitx.h		\
	$(NULL)

BUILT_SOURCES += 		\
	fcitx-keyboard.c	\
	fcitx-keyboard.h	\
	fcitx-inputmethod.c	\
	fcitx-inputmethod.h	\
	$(NULL)

fcitx-keyboard.h: fcitx-keyboard.c
fcitx-keyboard.c: $(srcdir)/org.fcitx.Fcitx.Keyboard.xml
	$(AM_V_GEN) gdbus-codegen --interface-prefix org.fcitx.Fcitx \
	                          --generate-c-code fcitx-keyboard \
	                          --c-namespace Fcitx $<

fcitx-inputmethod.h: fcitx-inputmethod.c
fcitx-inputmethod.c: $(srcdir)/org.fcitx.Fcitx.InputMethod.xml
	$(AM_V_GEN) gdbus-codegen --interface-prefix org.fcitx.Fcitx \
	                          --generate-c-code fcitx-inputmethod \
	                          --c-namespace Fcitx $<
endif

libkeyboard_la_SOURCES = 	\
	gsd-keyboard-plugin.c	\
	gsd-keyboard-manager.h	\
	gsd-keyboard-manager.c	\
	$(FCITX_SOURCES)	\
	$(BUILT_SOURCES)	\
	$(NULL)

libkeyboard_la_CPPFLAGS = \
	-I$(top_srcdir)/gnome-settings-daemon		\
	-I$(top_srcdir)/data				\
	-I$(top_srcdir)/plugins/common			\
	-DDATADIR=\""$(pkgdatadir)"\"			\
	-DLIBEXECDIR=\""$(libexecdir)"\"		\
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\" \
	$(AM_CPPFLAGS)

libkeyboard_la_CFLAGS = \
	$(PLUGIN_CFLAGS)		\
	$(SETTINGS_PLUGIN_CFLAGS)	\
	$(KEYBOARD_CFLAGS)		\
	$(AM_CFLAGS)

libkeyboard_la_LDFLAGS = 	\
	$(GSD_PLUGIN_LDFLAGS)	\
	$(NULL)

libkeyboard_la_LIBADD  =				\
	$(top_builddir)/plugins/common/libcommon.la	\
	$(SETTINGS_PLUGIN_LIBS)				\
	$(XF86MISC_LIBS)				\
	$(KEYBOARD_LIBS)				\
	$(NULL)

libexec_PROGRAMS = usd-test-keyboard
usd_test_keyboard_SOURCES =	\
	test-keyboard.c		\
	gsd-keyboard-manager.h	\
	gsd-keyboard-manager.c	\
	$(FCITX_SOURCES)	\
	$(BUILT_SOURCES)	\
	$(NULL)

usd_test_keyboard_CFLAGS = $(libkeyboard_la_CFLAGS)
usd_test_keyboard_CPPFLAGS = $(libkeyboard_la_CPPFLAGS)
usd_test_keyboard_LDADD = $(libkeyboard_la_LIBADD) $(top_builddir)/gnome-settings-daemon/libgsd.la

plugin_in_files = 		\
	keyboard.gnome-settings-plugin.in	\
	$(NULL)

plugin_DATA = $(plugin_in_files:.gnome-settings-plugin.in=.gnome-settings-plugin)

if HAVE_IBUS
noinst_PROGRAMS = test-keyboard-ibus-utils
test_keyboard_ibus_utils_SOURCES = test-keyboard-ibus-utils.c $(FCITX_SOURCES) $(BUILT_SOURCES)
test_keyboard_ibus_utils_CFLAGS = $(libkeyboard_la_CFLAGS)
test_keyboard_ibus_utils_CPPFLAGS = $(libkeyboard_la_CPPFLAGS)
test_keyboard_ibus_utils_LDADD = $(libkeyboard_la_LIBADD) $(top_builddir)/gnome-settings-daemon/libgsd.la

check-local: test-keyboard-ibus-utils
	$(builddir)/test-keyboard-ibus-utils > /dev/null
endif

EXTRA_DIST = 			\
	$(icons_DATA)		\
	$(plugin_in_files)	\
	$(ui_DATA)		\
	$(NULL)

CLEANFILES = 			\
	$(plugin_DATA)		\
	$(NULL)

DISTCLEANFILES =		\
	$(plugin_DATA)		\
	$(NULL)

@GSD_INTLTOOL_PLUGIN_RULE@
