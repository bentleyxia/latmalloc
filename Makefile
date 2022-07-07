CLEANEXTS = o so

SOURCE = src/latmalloc.c
OUTPUT_FILE = lib/liblatmalloc.so

.PHONY: all
all: $(OUTPUT_FILE)

$(OUTPUT_FILE): $(SOURCE)
	mkdir -p lib
	$(CC) -shared -fPIC -o $(OUTPUT_FILE) $(LDFLAGS) -o $@ $^

# .PHONY: install
# install:
	# mkdir -p $(INSTALL_DIR)
	# cp -p $(OUTPUT_FILE) $(DESTDIR)

.PHONY: clean
clean:
	for file in $(CLEANEXTS); do rm -f *.$$file; rm -rf lib; done
