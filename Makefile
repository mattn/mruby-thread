BUILD_SCRIPT = ./test_build.rb

.PHONY : test
test:
	ruby ${BUILD_SCRIPT} test

.PHONY : all
all:
	echo "NOOP"

.PHONY : clean
clean:
	ruby ${BUILD_SCRIPT} clean
