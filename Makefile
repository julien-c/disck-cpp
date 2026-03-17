CXX = c++
CXXFLAGS = -std=c++17 -Wall

all: hf-cache-dirs download

hf-cache-dirs: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

download: download.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lcurl

format:
	clang-format -i main.cpp download.cpp

clean:
	rm -f hf-cache-dirs download

.PHONY: all clean format
