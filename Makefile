# tier0 -- Linux shared library Makefile (GoldSrc tier0.so)
# Valve style: common/port.h defines _LINUX / POSIX shims
# Builds tier0.so with -fPIC -shared, matching Windows tier0.dll 314 exports logic
# On Linux only a subset compiles cleanly without full Win32 port; platform.cpp is the primary cross-platform unit.

CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas
CXXFLAGS += -fPIC
CPPFLAGS ?= -D_LINUX -DPOSIX -DLINUX -D_POSIX -DTIER0_DLL_EXPORT -DNDEBUG -D_CRT_SECURE_NO_WARNINGS
INCLUDES  = -Ipublic -Ipublic/tier0 -Itier0
LDFLAGS  += -shared -fPIC
LDLIBS   ?= -ldl -lpthread -lm -lrt

SRCDIR   = tier0
PUBDIR   = public
OUTDIR   = build
TARGET   = $(OUTDIR)/tier0.so

# Full tier0 sources — all tier0/*.cpp (100% 1:1, включая testthread).
SRCS     = $(wildcard $(SRCDIR)/*.cpp)

OBJS     = $(patsubst $(SRCDIR)/%.cpp,$(OUTDIR)/%.o,$(SRCS))

all: $(TARGET)

$(OUTDIR):
	@mkdir -p $(OUTDIR)

$(OUTDIR)/%.o: $(SRCDIR)/%.cpp | $(OUTDIR)
	@echo "CXX $<"
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS) | $(OUTDIR)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	@echo "BUILD OK: $@"

clean:
	@rm -rf $(OUTDIR)/*.o $(TARGET)

.PHONY: all clean
