all: m17-packet-encode m17-packet-decode

m17-packet-encode: m17-packet-encode.c
	gcc -I ../../libm17 -O2 -Wall -Wextra m17-packet-encode.c -o m17-packet-encode -lm -lm17 -lsndfile

m17-packet-decode: m17-packet-decode.c
	gcc -I ../../libm17 -O2 -Wall -Wextra m17-packet-decode.c -o m17-packet-decode -lm -lm17 -lsndfile

install: all
	sudo install m17-packet-encode /usr/local/bin
	sudo install m17-packet-decode /usr/local/bin

clean:
	rm -f m17-packet-encode m17-packet-decode
