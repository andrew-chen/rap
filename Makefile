CXX      ?= clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic -Werror

all: parse_run

parse_run: parse_run.o
	$(CXX) $(CXXFLAGS) -o $@ $^

parse_run.o: parse_run.cpp sexp_parser.hpp core.hpp intern.hpp arena.hpp
	$(CXX) $(CXXFLAGS) -c parse_run.cpp

clean:
	rm -f parse_run.o parse_run

run: parse_run
	./parse_run

.PHONY: all clean run
