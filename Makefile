CFLAGS ?= -Wall -I $(LIBFLUSH) -I $(CALIBRATE_DIR)
LDFLAGS ?= -L $(LIBFLUSH)/build/$(ARCH)/release -l flush -L bin -l shared_lib
ODIR ?= obj
BINDIR ?= bin

ARCH ?= x86
ARMAGEDDON ?= armageddon
CALIBRATE_DIR = $(ARMAGEDDON)/cache_template_attacks/cache_template_attack
LIBFLUSH = $(ARMAGEDDON)/libflush
LIBFLUSH_CONFIGURATION ?= i7-14700k

_MAIN = test_libflush
_HELPER = access_shared_mem
_SHARED = libshared_lib.so
_BINARIES = $(_SHARED) $(_HELPER) $(_MAIN)

MAIN = $(patsubst %, $(BINDIR)/%, $(_MAIN))
HELPER = $(patsubst %, $(BINDIR)/%, $(_HELPER))
SHARED = $(patsubst %, $(BINDIR)/%, $(_SHARED))
BINARIES = $(patsubst %, $(BINDIR)/%, $(_BINARIES))
OBJS = $(patsubst %,$(ODIR)/%.o,$(_MAIN) $(_HELPER)) $(patsubst lib%.so,$(ODIR)/%.o,$(_SHARED))

.PHONY: all $(LIBFLUSH)
all: $(BINARIES)

$(HELPER): $(BINDIR)/%: $(ODIR)/%.o $(BINDIR)
	$(CC) $(CFLAGS) $(ODIR)/$*.o -o $@ $(LDADD) $(LDFLAGS)

$(MAIN): $(BINDIR)/test_%: $(LIBFLUSH) $(ODIR)/test_%.o $(ODIR)/calibrate.o $(BINDIR)
	$(CC) $(CFLAGS) $(ODIR)/calibrate.o $(ODIR)/test_$*.o -o $@ $(LDADD) $(LDFLAGS)

$(SHARED): $(BINDIR)/lib%.so: $(ODIR) $(BINDIR)
	$(CC) $(CFLAGS) -c -fpic -o $(ODIR)/$*.o  $*.c
	$(CC) $(CFLAGS) -shared -o $@ $(ODIR)/$*.o

$(LIBFLUSH):
	$(MAKE) ARCH=$(ARCH) DEVICE_CONFIGURATION=$(LIBFLUSH_CONFIGURATION) HAVE_PAGEMAP_ACCESS=1 -C $@

.PHONY: clean run
clean:
	rm -rf $(BINDIR) $(ODIR)

$(ODIR)/%.o: %.c $(ODIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(ODIR)/calibrate.o: $(CALIBRATE_DIR)/calibrate.c $(ODIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(ODIR):
	mkdir -p $(ODIR)

$(BINDIR):
	mkdir -p $(BINDIR)
