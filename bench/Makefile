# Host-side unit tests for the TX equaliser. Compiles the firmware's own tx_eq.c unchanged.
SRC := ../stm32/aioc-fw/Src
CFLAGS := -O2 -g -Wall -Wextra -std=gnu11 -I$(SRC) -fsanitize=undefined,address -fno-sanitize-recover=all

.PHONY: test clean
test: test_tx_eq
	./test_tx_eq

test_tx_eq: test_tx_eq.c $(SRC)/tx_eq.c $(SRC)/tx_eq.h
	$(CC) $(CFLAGS) -o $@ test_tx_eq.c $(SRC)/tx_eq.c -lm

clean:
	rm -f test_tx_eq
