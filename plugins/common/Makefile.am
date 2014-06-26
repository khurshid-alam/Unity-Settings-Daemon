plugin_name = common

noinst_LTLIBRARIES = libcommon.la librr.la
libexec_PROGRAMS = check_gl_texture_size

libcommon_la_SOURCES = \
	gsd-keygrab.c		\
	gsd-keygrab.h		\
	gsd-input-helper.c	\
	gsd-input-helper.h

libcommon_la_CPPFLAGS = \
	$(AM_CPPFLAGS)

libcommon_la_CFLAGS = \
	$(PLUGIN_CFLAGS)		\
	$(SETTINGS_PLUGIN_CFLAGS)	\
	$(COMMON_CFLAGS)		\
	$(AM_CFLAGS)

libcommon_la_LDFLAGS = \
	$(GSD_PLUGIN_LDFLAGS)

libcommon_la_LIBADD  = \
	$(SETTINGS_PLUGIN_LIBS)		\
	$(COMMON_LIBS)

librr_la_SOURCES =		\
	gsd-pnp-ids.c \
	gsd-pnp-ids.h \
	gsd-rr.c \
	gsd-rr.h \
	gsd-rr-config.c \
	gsd-rr-config.h \
	gsd-rr-output-info.c \
	gsd-rr-private.h \
	display-name.c \
	edid-parse.c \
	edid.h

librr_la_CFLAGS = 		\
	-DLIBEXECDIR=\""$(libexecdir)\""			\
	-DPNP_IDS=\""$(datadir)/hwdata/pnp.ids"\"			\
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\"        \
	$(LIBRR_CFLAGS)

librr_la_LIBADD = 		\
	-lm \
	$(LIBRR_LIBS)

check_gl_texture_size_CPPFLAGS = \
	$(CHECK_GL_TEXTURE_SIZE_CFLAGS)

check_gl_texture_size_LDADD = \
	$(CHECK_GL_TEXTURE_SIZE_LIBS)

libexec_PROGRAMS = usd-test-input-helper

usd_test_input_helper_SOURCES = test-input-helper.c
usd_test_input_helper_LDADD = libcommon.la
usd_test_input_helper_CFLAGS = $(libcommon_la_CFLAGS)

noinst_PROGRAMS = test-egg-key-parsing

test_egg_key_parsing_SOURCES = test-egg-key-parsing.c
test_egg_key_parsing_LDADD = libcommon.la $(COMMON_LIBS)
test_egg_key_parsing_CFLAGS = $(libcommon_la_CFLAGS)

scriptsdir = $(datadir)/unity-settings-daemon-@GSD_API_VERSION@
scripts_DATA = input-device-example.sh

EXTRA_DIST = $(scripts_DATA) test-plugin.h
