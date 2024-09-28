test: task.cc
	$(CXX) -std=c++20 -Wall -O3 -o test $<
	# $(CXX) -std=c++20 -Wall -g -O1 -o test $<

clean:
	rm test

all: test
