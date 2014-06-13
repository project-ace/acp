PREFIX = $(HOME)/ACE/devel

SRCDIR = $(PREFIX)/acp/bl/ib
INCDIR = $(PREFIX)/acp/include
INSTALL_LIBDIR = $(HOME)/local/lib
INSTALL_INCDIR = $(HOME)/local/include
TESTDIR = $(PREFIX)/test/bl/ib

#OBJ_ACPBL_PRIM = $(HOME)/ACE.test/devel/acp/bl/ib/acpbl.o
OBJ_ACPBL_PRIM = $(HOME)/ACE/devel/acp/bl/ib/acpbl.o

CC = gcc
LD = gcc
CCFLAGS = -fPIC -O3 -I. -I$(INCDIR)
LIBFLAGS = -L$(INSTALL_LIBDIR) -libverbs
LDFLAGS = -fPIC -O3 $(LIBFLAGS)

LIBFLAGS = -L$(INSTALL_LIBDIR) -libverbs #-lacp
CCFLAGS = -Wall -I. -I$(INCDIR)
LDFLAGS = -Wall $(LIBFLAGS)
CCFLAGS = -Wall -fPIC -I. -I$(INCDIR)
LDFLAGS = -Wall -fPIC $(LIBFLAGS)

VER = 20140603
LIB = libacp.so
LIB_VER = $(LIB).$(VER)
LIB_REAL = $(INSTALL_LIBDIR)/$(LIB)
LIB_VER_REAL = $(INSTALL_LIBDIR)/$(LIB_VER)

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CCFLAGS) -c $<


###ALL: acpbl acpbl_ohandle
###
###acpbl:  acpbl.o acpbl_test.o
###	$(CC) $(TESTDIR)/acpbl_test.o $(SRCDIR)/acpbl.o -o $(TESTDIR)/acpbl $(LIB)
###
###acpbl_ohandle:  acpbl.o acpbl_ohandle.o
###	$(CC) $(TESTDIR)/acpbl_ohandle.o $(SRCDIR)/acpbl.o -o $(TESTDIR)/acpbl_ohandle $(LIB)
###
###acpbl.o:
###	$(CC) -c -I$(INCLUDEDIR) $(SRCDIR)/acpbl.c -o $(SRCDIR)/acpbl.o
###
###acpbl_test.o:
###	$(CC) -c -I$(INCLUDEDIR) $(TESTDIR)/acpbl_test.c -o $(TESTDIR)/acpbl_test.o
###
###acpbl_ohandle.o:
###	$(CC) -c -I$(INCLUDEDIR) $(TESTDIR)/acpbl_test_order_handle.c -o $(TESTDIR)/acpbl_ohandle.o
###
###clean: 
###	rm -f $(SRCDIR)/*.o $(TESTDIR)/*.o $(TESTDIR)/acpbl $(TESTDIR)/*.log