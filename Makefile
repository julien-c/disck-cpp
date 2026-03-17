CXX = c++
CXXFLAGS = -std=c++17 -Wall -g -O0

all: hf-cache-dirs download

hf-cache-dirs: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null || echo /usr/local/opt/openssl)

download: download.cpp vendor/httplib.h
	$(CXX) $(CXXFLAGS) -I$(OPENSSL_PREFIX)/include -L$(OPENSSL_PREFIX)/lib -o $@ $< -lssl -lcrypto

format:
	clang-format -i main.cpp download.cpp

clean:
	rm -f hf-cache-dirs download

.PHONY: all clean format
