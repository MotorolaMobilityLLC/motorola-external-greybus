LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := greybus
LOCAL_MODULE_TAGS := optional
LOCAL_ADDITIONAL_DEPENDENCIES := build-greybus
include $(BUILD_PHONY_PACKAGE)

GB_SRC_PATH := $(LOCAL_PATH)
GB_KDIRARG := KERNELDIR="${ANDROID_PRODUCT_OUT}/obj/KERNEL_OBJ"

TARGET_KERNEL_CROSS_COMPILE_PREFIX := $(strip $(TARGET_KERNEL_CROSS_COMPILE_PREFIX))
ifeq ($(TARGET_KERNEL_CROSS_COMPILE_PREFIX),)
GB_KERNEL_TOOLS_PREFIX := arm-eabi-
else
GB_KERNEL_TOOLS_PREFIX := $(TARGET_KERNEL_CROSS_COMPILE_PREFIX)
endif

GB_ARCHARG := ARCH=$(TARGET_ARCH)
GB_FLAGARG := EXTRA_CFLAGS+=-fno-pic
GB_ARGS := $(GB_KDIRARG) $(GB_ARCHARG) $(GB_FLAGARG)

#Create vendor/lib/modules directory if it doesn't exist
$(shell mkdir -p $(TARGET_OUT_VENDOR)/lib/modules)

ifeq ($(GREYBUS_DRIVER_INSTALL_TO_KERNEL_OUT),true)
GB_MODULES_OUT := $(KERNEL_MODULES_OUT)
else
GB_MODULES_OUT := $(TARGET_OUT_VENDOR)/lib/modules/
endif

# To ensure KERNEL_OUT and TARGET_PREBUILT_INT_KERNEL are defined,
# kernel/AndroidKernel.mk must be included. While m and regular
# make builds will include kernel/AndroidKernel.mk, mm and mmm builds
# do not. Therefore, we need to explicitly include kernel/AndroidKernel.mk.
# It is safe to include it more than once because the entire file is
# guarded by "ifeq ($(TARGET_PREBUILT_KERNEL),) ... endif".
TARGET_KERNEL_PATH := $(TARGET_KERNEL_SOURCE)/AndroidKernel.mk
include $(TARGET_KERNEL_PATH)

# Simply copy the kernel module from where the kernel build system
# created it to the location where the Android build system expects it.
# If LOCAL_MODULE_DEBUG_ENABLE is set, strip debug symbols. So that,
# the final images generated by ABS will have the stripped version of
# the modules
ifeq ($(TARGET_KERNEL_VERSION),3.18)
  MODULE_SIGN_FILE := perl ./$(TARGET_KERNEL_SOURCE)/scripts/sign-file
  MODSECKEY := $(KERNEL_OUT)/signing_key.priv
  MODPUBKEY := $(KERNEL_OUT)/signing_key.x509
else
  MODULE_SIGN_FILE := $(KERNEL_OUT)/scripts/sign-file
  MODSECKEY := $(KERNEL_OUT)/certs/signing_key.pem
  MODPUBKEY := $(KERNEL_OUT)/certs/signing_key.x509
endif

ifeq ($(GREYBUS_KERNEL_MODULE_SIG), true)
build-greybus: $(ACP) $(INSTALLED_KERNEL_TARGET)
	$(MAKE) clean -C $(GB_SRC_PATH)
	$(MAKE) -j$(MAKE_JOBS) -C $(GB_SRC_PATH) CROSS_COMPILE=$(GB_KERNEL_TOOLS_PREFIX) $(GB_ARGS)
	ko=`find $(GB_SRC_PATH) -type f -name "*.ko"`;\
	for i in $$ko;\
	do sh -c "\
	   KMOD_SIG_ALL=`cat $(KERNEL_OUT)/.config | grep CONFIG_MODULE_SIG_ALL | cut -d'=' -f2`; \
	   KMOD_SIG_HASH=`cat $(KERNEL_OUT)/.config | grep CONFIG_MODULE_SIG_HASH | cut -d'=' -f2 | sed 's/\"//g'`; \
	   if [ \"\$$KMOD_SIG_ALL\" = \"y\" ] && [ -n \"\$$KMOD_SIG_HASH\" ]; then \
	      echo \"Signing greybus module: \" `basename $$i`; \
	      $(MODULE_SIGN_FILE) \$$KMOD_SIG_HASH $(MODSECKEY) $(MODPUBKEY) $$i; \
	   fi; \
	"\
	$(GB_KERNEL_TOOLS_PREFIX)strip --strip-unneeded $$i;\
	$(ACP) -fp $$i $(GB_MODULES_OUT);\
	done
else
build-greybus: $(ACP) $(INSTALLED_KERNEL_TARGET)
	$(MAKE) clean -C $(GB_SRC_PATH)
	$(MAKE) -j$(MAKE_JOBS) -C $(GB_SRC_PATH) CROSS_COMPILE=$(GB_KERNEL_TOOLS_PREFIX) $(GB_ARGS)
	ko=`find $(GB_SRC_PATH) -type f -name "*.ko"`;\
	for i in $$ko;\
	do $(GB_KERNEL_TOOLS_PREFIX)strip --strip-unneeded $$i;\
	$(ACP) -fp $$i $(GB_MODULES_OUT);\
	done
endif
