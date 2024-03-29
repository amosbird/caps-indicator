CPPFLAGS = -MMD -MP
CXX ?= g++
LD = $(CXX)
LDFLAGS := $(CXXFLAGS) $(LDFLAGS) -lX11 -lxcb -lxcb-shape
PREFIX ?= /usr/local

caps-indicator: caps-indicator.o
	$(LD) $^ -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) -rf *.o *.d caps-indicator

install: caps-indicator
	install -Dm755 caps-indicator ${DESTDIR}${PREFIX}/bin/caps-indicator

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/caps-indicator

DEPS := caps-indicator.d
-include $(DEPS)

.PHONY: clean install uninstall
