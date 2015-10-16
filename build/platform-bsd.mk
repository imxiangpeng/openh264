include $(SRC_PATH)build/arch.mk
SHAREDLIBSUFFIX = so
SHAREDLIBSUFFIXVER=$(SHAREDLIBSUFFIX).$(SHAREDLIBVERSION)
SHLDFLAGS = -Wl,-soname,$(LIBPREFIX)$(PROJECT_NAME).$(SHAREDLIBSUFFIXVER)
CFLAGS += -fPIC
LDFLAGS += -lpthread
ifeq ($(ASM_ARCH), x86)
ifeq ($(ARCH), x86_64)
ASMFLAGS += -f elf64
else
ASMFLAGS += -f elf
endif
endif
