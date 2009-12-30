# Makefile for pluginlogger


CXXFLAGS=-DXP_UNIX -Wall -Werror -g -DPLUGIN=\"/home/ian/Projects/flashliar/libflashplayer.so\" -DLOGFILE=\"/tmp/plugin.log\"


all: install

install: plugin
	cp pluginlogger.so ~/.mozilla/plugins/

plugin: pluginlogger.so

pluginlogger.o: pluginlogger.cpp

pluginlogger.so: pluginlogger.o
	${CC} -shared -o $@ $< ${LDFLAGS}

clean: rm -f pluginlogger.so pluginlogger.o
