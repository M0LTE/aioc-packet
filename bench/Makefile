# Host-side unit tests for the TX and RX equalisers, the settings page loader, the reset
# diagnostics and the audio block helpers. Compiles the firmware's own tx_eq.c, rx_eq.c,
# settings_page.c, diag.c and audio_block.c unchanged. Run from this directory
# (make -C bench test): test_rx_eq reads its recordings from data/.
SRC := ../stm32/aioc-fw/Src
CFLAGS := -O2 -g -Wall -Wextra -std=gnu11 -I$(SRC) -fsanitize=undefined,address -fno-sanitize-recover=all

TESTS := test_tx_eq test_rx_eq test_rx_eq_core test_settings_page test_settings_page_off test_diag test_audio_block

.PHONY: test clean
test: $(TESTS)
	./test_tx_eq
	python3 rxeq_fixed.py check
	./test_rx_eq
	./test_rx_eq_core
	./test_settings_page
	./test_settings_page_off
	./test_diag
	./test_audio_block

test_tx_eq: test_tx_eq.c $(SRC)/tx_eq.c $(SRC)/tx_eq.h
	$(CC) $(CFLAGS) -o $@ test_tx_eq.c $(SRC)/tx_eq.c -lm

# Once as the firmware has it, once without the DC stage (RXEQ_NO_DC_STAGE); both must match
# bench/rxeq_fixed.py bit for bit (rxeq_fixed.py check confirms the test data is its output)
test_rx_eq: test_rx_eq.c $(SRC)/rx_eq.c $(SRC)/rx_eq.h $(SRC)/rx_eq_k5_fixed.h
	$(CC) $(CFLAGS) -o $@ test_rx_eq.c $(SRC)/rx_eq.c -lm

test_rx_eq_core: test_rx_eq.c $(SRC)/rx_eq.c $(SRC)/rx_eq.h $(SRC)/rx_eq_k5_fixed.h
	$(CC) $(CFLAGS) -DRXEQ_NO_DC_STAGE -o $@ test_rx_eq.c $(SRC)/rx_eq.c -lm

PAGE_DEPS := test_settings_page.c $(SRC)/settings_page.c $(SRC)/settings_page.h $(SRC)/settings.h

# Once as the release builds it (EQ defaults to k5-red), once as make TXEQ_DEFAULT=off
test_settings_page: $(PAGE_DEPS)
	$(CC) $(CFLAGS) -o $@ test_settings_page.c $(SRC)/settings_page.c

test_settings_page_off: $(PAGE_DEPS)
	$(CC) $(CFLAGS) -DTXEQ_DEFAULT_OFF -o $@ test_settings_page.c $(SRC)/settings_page.c

test_diag: test_diag.c $(SRC)/diag.c $(SRC)/diag.h $(SRC)/settings.h
	$(CC) $(CFLAGS) -o $@ test_diag.c $(SRC)/diag.c

test_audio_block: test_audio_block.c $(SRC)/audio_block.c $(SRC)/audio_block.h
	$(CC) $(CFLAGS) -o $@ test_audio_block.c $(SRC)/audio_block.c

clean:
	rm -f $(TESTS)
