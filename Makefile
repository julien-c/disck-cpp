CXX = c++
CXXFLAGS = -std=c++17 -Wall

hf-cache-dirs: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f hf-cache-dirs

.PHONY: clean
