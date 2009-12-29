# Makefile for flashliar


CXXFLAGS=-DXP_UNIX -Wall -Werror -g

all: install

install: plugin
	cp flashliar.so ~/.mozilla/plugins/

plugin: flashliar.so

flashliar.o: flashliar.cpp

flashliar.so: flashliar.o
	${CC} -shared -o $@ $< ${LDFLAGS}
