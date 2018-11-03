HOST_GCC=g++-7
TARGET_GCC=arm-none-eabi-gcc
GCCPLUGINS_DIR:= $(shell $(TARGET_GCC) -print-file-name=plugin)
CXXFLAGS+= \
	-I$(GCCPLUGINS_DIR)/include \
	-std=c++11 -fPIC -fno-lto -fno-rtti -Wall -Wno-literal-suffix

all: nexmon.so

nexmon.so: nexmon.o
	$(HOST_GCC) -shared -o $@ $<

nexmon.o: nexmon.c
	$(HOST_GCC) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f *.o *.so
