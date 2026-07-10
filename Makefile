CC ?= gcc
PKG_CONFIG ?= pkg-config
CPPFLAGS ?=
CFLAGS ?= -Wall -Wextra -O2
LDFLAGS ?=

CORE_CPPFLAGS = $(CPPFLAGS) -I/usr/include/freetype2
CORE_CFLAGS = $(CFLAGS)
CORE_LIBS ?= -lgpiod -lfreetype -lasound -lmpg123 -pthread

MHD_CFLAGS := $(shell $(PKG_CONFIG) --cflags libmicrohttpd 2>/dev/null)
MHD_LIBS := $(shell $(PKG_CONFIG) --libs libmicrohttpd 2>/dev/null)
API_CPPFLAGS = $(CPPFLAGS) $(MHD_CFLAGS) -I/usr/include/freetype2 -I/usr/include/libpng16
API_CFLAGS = $(CFLAGS)
API_LIBS ?= $(if $(strip $(MHD_LIBS)),$(MHD_LIBS),-lmicrohttpd) -lpng -lfreetype -lmpg123 -lmp3lame -pthread

.PHONY: all clean install uninstall

all: mk-piclock-core mk-piclock-api

mk-piclock-core: mk-piclock.c led_control.c util.c ipc_protocol.h asset_format.h led_control.h util.h
	$(CC) $(CORE_CPPFLAGS) $(CORE_CFLAGS) mk-piclock.c led_control.c util.c $(LDFLAGS) $(CORE_LIBS) -o $@

mk-piclock-api: mk-piclock-api.c asset_store.c music_jobs.c util.c ipc_protocol.h asset_format.h asset_store.h music_jobs.h util.h
	$(CC) $(API_CPPFLAGS) $(API_CFLAGS) mk-piclock-api.c asset_store.c music_jobs.c util.c $(LDFLAGS) $(API_LIBS) -o $@


install: all
	-sudo systemctl stop mk-piclock-api.service
	@getent group mk-piclock >/dev/null || sudo groupadd --system mk-piclock
	@id -u mk-piclock-core >/dev/null 2>&1 || sudo useradd --system --gid mk-piclock --home-dir /nonexistent --shell /usr/sbin/nologin mk-piclock-core
	@id -u mk-piclock-api >/dev/null 2>&1 || sudo useradd --system --gid mk-piclock --home-dir /nonexistent --shell /usr/sbin/nologin mk-piclock-api
	@for group in audio spi gpio; do getent group $$group >/dev/null && sudo usermod -a -G $$group mk-piclock-core || true; done
	sudo mkdir -p /opt/mk-piclock/assets/images \
		/opt/mk-piclock/assets/bedtime-images \
		/opt/mk-piclock/assets/music \
		/opt/mk-piclock/assets/music/.processing \
		/opt/mk-piclock/assets/stories \
		/opt/mk-piclock/assets/fonts \
		/opt/mk-piclock/config
	sudo chown -R mk-piclock-api:mk-piclock /opt/mk-piclock/assets
	sudo chmod -R u=rwX,g=rX,o= /opt/mk-piclock/assets
	sudo chown -R mk-piclock-core:mk-piclock /opt/mk-piclock/config
	sudo chmod -R u=rwX,g=rwX,o= /opt/mk-piclock/config
	sudo install -m 0755 mk-piclock-core /opt/mk-piclock/mk-piclock-core
	sudo install -m 0755 mk-piclock-api /opt/mk-piclock/mk-piclock-api
	sudo install -m 0640 -o root -g mk-piclock assets/default-alarm.mp3 /opt/mk-piclock/assets/default-alarm.mp3
	sudo install -m 0640 -o root -g mk-piclock assets/message-chime.mp3 /opt/mk-piclock/assets/message-chime.mp3
	sudo rm -rf /opt/mk-piclock/web /opt/mk-piclock/api
	sudo install -d -m 0755 /opt/mk-piclock/web /opt/mk-piclock/api
	sudo cp -a web/. /opt/mk-piclock/web/
	sudo chown -R root:root /opt/mk-piclock/web
	sudo chmod -R a=rX /opt/mk-piclock/web
	sudo install -m 0644 api/openapi-v1.json /opt/mk-piclock/api/openapi-v1.json
	sudo install -m 0644 mk-piclock-core.service /etc/systemd/system/mk-piclock-core.service
	sudo install -m 0644 mk-piclock-api.service /etc/systemd/system/mk-piclock-api.service
	sudo systemctl daemon-reload
	sudo systemctl enable mk-piclock-core.service mk-piclock-api.service
	@echo "Installed."
	@echo "Start with: sudo systemctl restart mk-piclock-core mk-piclock-api"

uninstall:
	-sudo systemctl disable --now mk-piclock-api.service mk-piclock-core.service
	sudo rm -f /etc/systemd/system/mk-piclock-api.service /etc/systemd/system/mk-piclock-core.service
	sudo rm -f /opt/mk-piclock/mk-piclock-api /opt/mk-piclock/mk-piclock-core /opt/mk-piclock/assets/default-alarm.mp3 /opt/mk-piclock/assets/message-chime.mp3
	sudo rm -rf /opt/mk-piclock/web /opt/mk-piclock/api
	sudo systemctl daemon-reload

clean:
	rm -f mk-piclock-core mk-piclock-api
