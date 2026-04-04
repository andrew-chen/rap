CXX      ?= clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic -Werror

all: parse_run test_rap

parse_run: parse_run.o
	$(CXX) $(CXXFLAGS) -o $@ $^

parse_run.o: parse_run.cpp core/sexp_parser.hpp core/core.hpp core/intern.hpp core/arena.hpp
	$(CXX) $(CXXFLAGS) -c parse_run.cpp

test_rap: rap/test_rap.o
	$(CXX) $(CXXFLAGS) -o $@ $^

rap/test_rap.o: rap/test_rap.cpp rap/rap.hpp rap/work_queue.hpp core/sexp_parser.hpp core/core.hpp core/intern.hpp core/arena.hpp
	$(CXX) $(CXXFLAGS) -c rap/test_rap.cpp -o rap/test_rap.o

clean:
	rm -f parse_run.o parse_run rap/test_rap.o test_rap

run: parse_run
	./parse_run

.PHONY: all clean run
