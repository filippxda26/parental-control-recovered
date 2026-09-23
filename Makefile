CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
all: parental-control parental-control-web
parental-control: src/parental-control.c
	$(CC) $(CFLAGS) -o $@ $< -ljson-c
parental-control-web: src/parental-control-web.c
	$(CC) $(CFLAGS) -o $@ $< -ljson-c
clean:
	rm -f parental-control parental-control-web
