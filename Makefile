test: task.cc
	# $(CXX) -std=c++20 -Wall -g -O0 -o test $<
	# $(CXX) -std=c++20 -Wall -g -O1 -fsanitize=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope -fno-optimize-sibling-calls -o test $<
	# $(CXX) -std=c++20 -Wall -g -O2 -fsanitize=thread -o test $< # with sudo sysctl vm.mmap_rnd_bits=30
	$(CXX) -std=c++20 -Wall -O3 -o test $<

clean:
	rm -f test

all: test
