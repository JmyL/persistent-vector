test: task.cc
	$(CXX) -Wall -o test $<

clean:
	rm test

all: test
