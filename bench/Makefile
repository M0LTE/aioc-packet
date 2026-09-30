# Host-side unit tests for the TX equaliser and the settings page loader. Compiles the
# firmware's own tx_eq.c and settings_page.c unchanged.
SRC := ../stm32/aioc-fw/Src
CFLAGS := -O2 -g -Wall -Wextra -std=gnu11 -I$(SRC) -fsanitize=undefined,address -fno-sanitize-recover=all

TESTS := test_tx_eq test_settings_page test_settings_page_off

.PHONY: test clean
test: $(TESTS)
	./test_tx_eq
	./test_settings_page
	./test_settings_page_off

test_tx_eq: test_tx_eq.c $(SRC)/tx_eq.c $(SRC)/tx_eq.h
	$(CC) $(CFLAGS) -o $@ test_tx_eq.c $(SRC)/tx_eq.c -lm

PAGE_DEPS := test_settings_page.c $(SRC)/settings_page.c $(SRC)/settings_page.h $(SRC)/settings.h

# Once as the release builds it (EQ defaults to k5-red), once as make TXEQ_DEFAULT=off
test_settings_page: $(PAGE_DEPS)
	$(CC) $(CFLAGS) -o $@ test_settings_page.c $(SRC)/settings_page.c

test_settings_page_off: $(PAGE_DEPS)
	$(CC) $(CFLAGS) -DTXEQ_DEFAULT_OFF -o $@ test_settings_page.c $(SRC)/settings_page.c

clean:
	rm -f $(TESTS)
