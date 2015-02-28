BINDIR=	/etc
CFLAGS=-O

OBJS = holed.o

holed: ${OBJS}
	cc ${LDFLAGS} -o $@ ${OBJS}

install: holed
	cp holed ${BINDIR}

clean:
	rm ${OBJS} holed
