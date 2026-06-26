############################################################################
#
# Copyright (C) 2025 Xiaomi Corporation
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

R3D_DIR = $(APPDIR)/frameworks/graphics/animengine/r3d

# r3d 公共头
CFLAGS += ${INCDIR_PREFIX}$(R3D_DIR)/include

# vg_lite headers (reuse the same Kconfig path as LVGL)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../$(CONFIG_LV_DRAW_VG_LITE_INCLUDE)

# 核心引擎(后端无关)
CSRCS += $(R3D_DIR)/core/r3d_math.c
CSRCS += $(R3D_DIR)/core/r3d_model.c
CSRCS += $(R3D_DIR)/core/r3d_anim.c
CSRCS += $(R3D_DIR)/core/r3d_deform.c
CSRCS += $(R3D_DIR)/core/r3d_skin.c
CSRCS += $(R3D_DIR)/core/r3d_backend.c

# 后端：null 始终编入(占位/分发链路)
CSRCS += $(R3D_DIR)/backend/null/backend_null.c

# VGLite 后端按配置选择
ifneq ($(CONFIG_R3D_BACKEND_VGLITE),)
CFLAGS += -DR3D_BACKEND_VGLITE
CSRCS += $(R3D_DIR)/backend/vglite/backend_vglite.c
endif

# 设备运行时封装
CSRCS += $(R3D_DIR)/engine/r3d_engine.c

# Standalone demo program (builtin app)，按配置注册
ifneq ($(CONFIG_R3D_VGLITE_DEMO),)
PRIORITY = $(CONFIG_R3D_VGLITE_DEMO_PRIORITY)
STACKSIZE = $(CONFIG_R3D_VGLITE_DEMO_STACKSIZE)

PROGNAME += r3d_vglite_demo
MAINSRC += $(R3D_DIR)/demo/r3d_vglite_demo.c
endif

ASRCS := $(wildcard $(ASRCS))
CSRCS := $(wildcard $(CSRCS))
CXXSRCS := $(wildcard $(CXXSRCS))
MAINSRC := $(wildcard $(MAINSRC))
NOEXPORTSRCS = $(ASRCS)$(CSRCS)$(CXXSRCS)$(MAINSRC)

ifneq ($(NOEXPORTSRCS),)
BIN := $(APPDIR)/staging/libframework.a
endif

include $(APPDIR)/Application.mk
