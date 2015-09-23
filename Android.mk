# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)

bsdiff_common_cflags = -Wall -Werror -Wextra -Wno-unused-parameter

include $(CLEAR_VARS)
LOCAL_MODULE := bsdiff
LOCAL_SRC_FILES := \
    bsdiff.c \
    bsdiff_main.c
LOCAL_CFLAGS := $(bsdiff_common_cflags)
LOCAL_C_INCLUDES += external/bzip2
LOCAL_STATIC_LIBRARIES := libbz
LOCAL_SHARED_LIBRARIES := \
    libdivsufsort64 \
    libdivsufsort
include $(BUILD_HOST_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := bspatch
LOCAL_SRC_FILES := \
    bspatch.c \
    bspatch_main.c \
    exfile.c \
    extents.c
LOCAL_CFLAGS := $(bsdiff_common_cflags)
LOCAL_C_INCLUDES += external/bzip2
LOCAL_STATIC_LIBRARIES := libbz
include $(BUILD_HOST_EXECUTABLE)
