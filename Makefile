.PHONY: all clean it

CC=gcc
INC=-I./deps/nghttp2/lib/includes
LIB=./deps/nghttp2/lib/.libs
CFLAGS=-Wall -Wextra -Wno-unused-parameter $(INC)
LDFLAGS=-Wl,-Bstatic -lnghttp2 -Wl,-Bdynamic -lssl -lcrypto

all: apns2-test

apns2-test: apns2-test.c $(LIB)/libnghttp2.a
	$(CC) -o apns2-test apns2-test.c $(CFLAGS) $(LDFLAGS)
		
$(LIB)/libnghttp2.a:
	git submodule update --init
	cd deps/nghttp2 && \
	autoreconf -i && \
	automake && autoconf && \
	./configure --disable-python-bindings && \
	make -C lib/ && \
	rm integration-tests/setenv 

clean:
	rm -f *.o apns2-test
	
it:
	@echo test
