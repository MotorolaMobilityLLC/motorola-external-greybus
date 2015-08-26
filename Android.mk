LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := greybus
LOCAL_MODULE_TAGS := optional
LOCAL_ADDITIONAL_DEPENDENCIES := build-greybus
include $(BUILD_PHONY_PACKAGE)

GB_SRC_PATH := $(LOCAL_PATH)
GB_KDIRARG := KERNELDIR="${ANDROID_PRODUCT_OUT}/obj/KERNEL_OBJ"
ifeq ($(TARGET_ARCH),arm64)
  GB_KERNEL_TOOLS_PREFIX=aarch64-linux-android-
else
  GB_KERNEL_TOOLS_PREFIX:=arm-eabi-
endif
GB_ARCHARG := ARCH=$(TARGET_ARCH)
GB_FLAGARG := EXTRA_CFLAGS+=-fno-pic
GB_ARGS := $(GB_KDIRARG) $(GB_ARCHARG) $(GB_FLAGARG)

build-greybus: $(ACP) $(INSTALLED_KERNEL_TARGET)
	$(MAKE) clean -C $(GB_SRC_PATH)
	$(MAKE) -j$(MAKE_JOBS) -C $(GB_SRC_PATH) CROSS_COMPILE=$(GB_KERNEL_TOOLS_PREFIX) $(GB_ARGS)
	ko=`find $(GB_SRC_PATH) -type f -name "*.ko"`;\
	for i in $$ko;\
	do $(GB_KERNEL_TOOLS_PREFIX)strip --strip-unneeded $$i;\
	$(ACP) -fp $$i $(TARGET_OUT)/lib/modules/;\
	done
