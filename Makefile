CXX = c++
CXXFLAGS = -std=c++17 -Wall

hf-cache-dirs: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

format:
	clang-format -i main.cpp

clean:
	rm -f hf-cache-dirs

.PHONY: clean format
