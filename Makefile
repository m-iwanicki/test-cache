CFLAGS ?= -Wall -I $(LIBFLUSH) -I $(CALIBRATE_DIR)
LDFLAGS ?= -L $(LIBFLUSH)/build/$(ARCH)/release -l flush
ODIR = obj
BINDIR ?= bin

ARCH ?= x86
ARMAGEDDON ?= armageddon
CALIBRATE_DIR = $(ARMAGEDDON)/cache_template_attacks/cache_template_attack
LIBFLUSH = $(ARMAGEDDON)/libflush
LIBFLUSH_CONFIGURATION ?= i7-14700k

MAIN = test_original test_libflush
HELPER = access_shared_mem no_access_shared_mem
BINARIES = $(MAIN) $(HELPER)
_OBJS = $(patsubst %, %.o, $(BINARIES))
OBJS = $(patsubst %,$(ODIR)/%,$(_OBJS))

.PHONY: all $(LIBFLUSH)
all: $(BINARIES)

$(MAIN): test_%: $(LIBFLUSH) $(ODIR)/test_%.o $(ODIR)/calibrate.o $(BINDIR)
	$(CC) $(CFLAGS) $(ODIR)/calibrate.o $(ODIR)/$@.o -o $(BINDIR)/$@ $(LDADD) $(LDFLAGS)

.SECONDEXPANSION:
$(HELPER): $(ODIR)/$$@.o $(BINDIR)
	$(CC) $(CFLAGS) $(ODIR)/$@.o -o $(BINDIR)/$@ $(LDADD) $(LDFLAGS) -Wl,--as-needed

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
