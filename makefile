MYNAME	= dvdreadfs
DEFINES	= -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64 -DFORCE_SINGLE_THREAD=1
LIBS	= -lfuse -ldvdread
OBJ	= $(MYNAME).o
CC	= gcc
LD	= $(CC)
CCOPTS	= -O -g
SRC	= $(MYNAME).c
BINDIR	= /usr/bin

$(MYNAME): $(OBJ)
	$(LD) -o $(MYNAME) $(OBJ) $(LIBS)

.c.o:
	$(CC) $(CCOPTS) $(DEFINES) -c $<

install: $(MYNAME)
	install $(MYNAME) $(BINDIR)

clean:
	rm -f $(OBJ) $(MYNAME) $(MYNAME).tar

tar:
	tar cf $(MYNAME).tar $(SRC) Makefile README
