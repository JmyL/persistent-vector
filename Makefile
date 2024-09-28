SANITIZER := -fno-omit-frame-pointer -fsanitize=address 
# -fsanitize=thread -fno-sanitize-recover=all

test: task.cc
	$(CXX) -std=c++20 -Wall -g -O0 -o test $<
	# $(CXX) -std=c++20 -Wall -g -O0 $(SANITIZER) -o test $<
	# $(CXX) -std=c++20 -Wall -O3 -o test $<

clean:
	rm test

all: test
