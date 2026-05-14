CXX      ?= clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic -Werror

all: parse_run test_rap security/security_test \
     core_test_extension rap_test_extension test_stage2

parse_run: parse_run.o
	$(CXX) $(CXXFLAGS) -o $@ $^

parse_run.o: parse_run.cpp core/sexp_parser.hpp core/core.hpp core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -c parse_run.cpp

test_rap: rap/test_rap.o
	$(CXX) $(CXXFLAGS) -o $@ $^

rap/test_rap.o: rap/test_rap.cpp rap/rap.hpp rap/work_queue.hpp core/sexp_parser.hpp core/core.hpp core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -c rap/test_rap.cpp -o rap/test_rap.o

security/security_test: security/security_test.cpp core/sexp_parser.hpp core/core.hpp core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

core_test_extension: core/test_extension.cpp \
    core/sexp_parser.hpp core/core.hpp core/intern.hpp \
    core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

rap_test_extension: rap/test_rap_extension.cpp \
    rap/rap.hpp rap/work_queue.hpp core/sexp_parser.hpp core/core.hpp \
    core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test_stage2: rap/test_stage2.cpp \
    rap/loop.hpp rap/agenda.hpp rap/spine.hpp rap/changeset.hpp \
    rap/rap.hpp core/sexp_parser.hpp core/core.hpp \
    core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f parse_run.o parse_run rap/test_rap.o test_rap security/security_test \
	      core_test_extension rap_test_extension test_stage2

run: parse_run
	./parse_run

test: all
	./parse_run
	./security/security_test
	./test_rap
	./core_test_extension
	./rap_test_extension
	./test_stage2
	@echo "All tests complete."

.PHONY: all clean run test
