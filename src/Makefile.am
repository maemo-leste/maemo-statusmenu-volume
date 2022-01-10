hildondesktoplib_LTLIBRARIES = volume_status_menu_item.la

volume_status_menu_item_la_CFLAGS = $(STATUS_MENU_VOLUME_CFLAGS) \
			 -DOSSOLOG_COMPILE=1 -DG_LOG_DOMAIN=\"$(PACKAGE)\" \
			 -DLOCALE_DIR=\"$(localedir)\"

volume_status_menu_item_la_LDFLAGS = -module -avoid-version
volume_status_menu_item_la_LIBADD = $(STATUS_MENU_VOLUME_LIBS) -lm

volume_status_menu_item_la_SOURCES = item.c

propertydir = $(datadir)/maemo-statusmenu-volume
property_DATA = sinks.ini

hildonstatusmenudesktopentry_DATA = volume.desktop

EXTRA_DIST = $(hildonstatusmenudesktopentry_DATA) $(property_DATA)

MAINTAINERCLEANFILES = Makefile.in

install-data-hook:
	@(cd $(DESTDIR)$(hildondesktoplibdir) && $(RM) \
		$(hildondesktoplib_LTLIBRARIES))
