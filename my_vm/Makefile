CC = gcc
CFLAGS = -g -c -Wall -Wextra -Wvla -Wno-error -m32
AR = ar -rc
RANLIB = ranlib

my_vm.a: my_vm.o
	$(AR) $@ $^
	$(RANLIB) $@

# Sanity tests, built natively (64-bit) with sanitizers: `make test`
test: my_vm.c my_vm.h tests/test_vm.c
	$(CC) -g -Wall -Wextra -fsanitize=address,undefined my_vm.c tests/test_vm.c -lm -o test_vm
	./test_vm

clean:
	rm -rf *.o *.a test_vm
