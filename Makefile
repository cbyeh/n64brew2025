V=1
SOURCE_DIR=src
BUILD_DIR=build
N64_CFLAGS += -O3 -DNDEBUG
include $(N64_INST)/include/n64.mk
N64_ROM_TITLE = "Junk Runner 64"
N64_ROM_REGION = E
# N64_ROM_REGION = P
N64_ROM_SAVETYPE = sram256k
include $(T3D_INST)/t3d.mk

MK_ASSET=$(N64_INST)/bin/mkasset

N64_C_AND_CXX_FLAGS += -Og
# JR_DEBUG=1: ISViewer log for emulator test runs
N64_C_AND_CXX_FLAGS += $(if $(JR_DEBUG),-DJR_DEBUG)

all: junkrunner64.z64
.PHONY: all

tests: junkrunner64_test.z64
.PHONY: tests

###
# fonts
###

FONT_SOURCES := $(shell find assets/ -type f -name '*.ttf' | sort)

FONTS := $(FONT_SOURCES:assets/%.ttf=filesystem/%.font64)

MKFONT_FLAGS ?=

filesystem/%.font64: assets/%.ttf
	@mkdir -p $(dir $@)
	@echo "    [FONT] $@"
	@$(N64_MKFONT) $(MKFONT_FLAGS) -o $(dir $@) "$<"

filesystem/fonts/DeltaBlock.font64: MKFONT_FLAGS=--monochrome -s 12

###
# images
###

PNG_RGBA16 := $(shell find assets/ -type f -name '*.png' | sort)

MKSPRITE_FLAGS ?=

SPRITES := $(PNG_RGBA16:assets/%.png=filesystem/%.sprite)

.SECONDEXPANSION:
filesystem/%.sprite: assets/%.png $$(wildcard assets/%.json)
	@mkdir -p $(dir $@)
	@echo "    [SPRITE] $@"
	$(N64_MKSPRITE) -f ${shell jq -r .format ${<:%.png=%.json} || echo AUTO} --compress -o "$(dir $@)" "$<"

###
# t3d_meshes
###

EXPORT_SOURCE := $(shell find tools/mesh_export/ -type f -name '*.py' | sort)
# BLENDER_4 := /home/james/Blender/blender-4.0.2-linux-x64/blender

MESH_SOURCES := $(shell find assets/meshes -type f -name '*.blend' | sort)

TMESHES := $(MESH_SOURCES:assets/meshes/%.blend=filesystem/meshes/%.tmesh)

filesystem/meshes/%.tmesh filesystem/meshes/%.anim: assets/meshes/%.blend $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	@mkdir -p $(dir $(@:filesystem/meshes/%.tmesh=build/assets/meshes/%.tmesh))
	$(BLENDER_4) $< --background --factory-startup --python-exit-code 1 --python tools/mesh_export/t3d_mesh.py -- $(@:filesystem/meshes/%.tmesh=build/assets/meshes/%.tmesh)
	$(MK_ASSET) -o $(dir $@) -w 256 $(@:filesystem/meshes/%.tmesh=build/assets/meshes/%.tmesh)
	-cp $(@:filesystem/meshes/%.tmesh=build/assets/meshes/%.anim) $(@:%.tmesh=%.anim)

assets/game_objects.json: tools/mesh_export/rebuild_prefabs.py $(MESH_SOURCES)
	$(BLENDER_4) --background --factory-startup --python-exit-code 1 --python tools/mesh_export/rebuild_prefabs.py -- assets/game_objects.json

###
# materials
###

MATERIAL_SOURCES := $(shell find assets/ -type f -name '*.mat.json' | sort)
MATERIAL_BLENDER_SOURCES := $(shell find assets/ -type f -name '*.mat.blend' | sort)

MATERIALS := $(MATERIAL_SOURCES:assets/%.mat.json=filesystem/%.mat) $(MATERIAL_BLENDER_SOURCES:assets/%.mat.blend=filesystem/%.mat)

filesystem/%.mat: assets/%.mat.json $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	@mkdir -p $(dir $(@:filesystem/%.mat=build/assets/%.mat))
	python3 tools/mesh_export/material.py --default assets/materials/default.mat.json $< $(@:filesystem/%.mat=build/assets/%.mat)
	$(MK_ASSET) -o $(dir $@) -w 4 $(@:filesystem/%.mat=build/assets/%.mat)

filesystem/%.mat: assets/%.mat.blend $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	$(BLENDER_4) $< --background --factory-startup --python-exit-code 1 --python tools/mesh_export/material_fast64.py -- $@

list_materials: $(MATERIALS)
	echo $(MATERIALS)

###
# sound effects
###

SOUND_EFFECT_SOURCES := $(shell find assets/ -type f -name '*.wav' | sort)

SOUND_EFFECTS := $(SOUND_EFFECT_SOURCES:assets/%.wav=filesystem/%.wav64)

filesystem/%.wav64: assets/%.wav $$(wildcard assets/%.txt)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	$(N64_AUDIOCONV) ${shell cat ${<:%.wav=%.txt} || echo --wav-compress 1 --wav-resample 22050} -o $@ $<
	
###
# music
###

MUSIC_SOURCES := $(shell find assets/ -type f -name '*.mp3' | sort)

MUSIC := $(MUSIC_SOURCES:assets/%.mp3=filesystem/%.wav64)

filesystem/%.wav64: assets/%.mp3 $$(wildcard assets/%.txt)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	$(N64_AUDIOCONV) ${shell cat ${<:%.mp3=%.txt} || echo --wav-compress 1 --wav-resample 22050} -o $@ $<

###
# material_builder
###

assets/materials/materials.blend: tools/mesh_export/material_generator.py $(MATERIAL_SOURCES)
	$(BLENDER_4) --background --factory-startup --python-exit-code 1 --python tools/mesh_export/material_generator.py -- $@ $(MATERIAL_SOURCES)

###
# cutscenes
###

SCRIPTS := $(shell find assets -type f -name '*.script' | sort)

SCRIPTS_COMPILED := $(SCRIPTS:assets/%=filesystem/%)

build/assets/scripts/globals.json build/assets/scripts/globals.dat src/player/inventory_mapping.c: tools/mesh_export/globals.py assets/scripts/globals.script
	@mkdir -p $(dir $@)
	python3 tools/mesh_export/globals.py build/assets/scripts/globals src/player/inventory_mapping.c assets/scripts/globals.script

filesystem/scripts/globals.dat: build/assets/scripts/globals.dat
	$(MK_ASSET) -o $(dir $@) -w 4 $<

filesystem/scripts/%.script: assets/scripts/%.script build/assets/scripts/globals.json $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	@mkdir -p $(dir $(@:filesystem/%=build/assets/%))
	python3 tools/mesh_export/cutscene.py -g build/assets/scripts/globals.json $< $(@:filesystem/%=build/assets/%)
	$(MK_ASSET) -o $(dir $@) -w 4 $(@:filesystem/%=build/assets/%)
	
filesystem/scenes/%.script: assets/scenes/%.script build/assets/scenes/%.json assets/scenes/%.blend build/assets/scripts/globals.json $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	@mkdir -p $(dir $(@:filesystem/%=build/assets/%))
	python3 tools/mesh_export/cutscene.py -g build/assets/scripts/globals.json -c $(@:filesystem/%.script=build/assets/%.json) $< $(@:filesystem/%=build/assets/%)
	$(MK_ASSET) -o $(dir $@) -w 4 $(@:filesystem/%=build/assets/%)

###
# scenes
###

SCENE_SOURCES := $(shell find assets/scenes -type f -name '*.blend' | sort)

SCENES := $(SCENE_SOURCES:assets/scenes/%.blend=filesystem/scenes/%.scene)

.SECONDEXPANSION:
filesystem/scenes/%.scene: assets/scenes/%.blend $$(wildcard assets/scenes/%.script) build/assets/scripts/globals.json $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	@mkdir -p $(dir $(@:filesystem/scenes/%.scene=build/assets/scenes/%.scene))
	echo $@ $<
	$(BLENDER_4) $< --background --factory-startup --python-exit-code 1 --python tools/mesh_export/scene.py -- $(@:filesystem/scenes/%.scene=build/assets/scenes/%.scene) $(@:%.scene=%.overworld)
	$(MK_ASSET) -o $(dir $@) -w 256 $(@:filesystem/scenes/%.scene=build/assets/scenes/%.scene)
	-cp $(@:filesystem/scenes/%.scene=build/assets/scenes/%.sanim) $(@:%.scene=%.sanim)

filesystem/scenes/%.overworld build/assets/scenes/%.json: filesystem/scenes/%.scene;

build/assets/scenes/%_exits.txt: assets/scenes/%.blend tools/mesh_export/find_exits.py
	@mkdir -p $(dir $@)
	$(BLENDER_4) $< --background --factory-startup --python-exit-code 1 --python tools/mesh_export/find_exits.py -- $@

all_exits: $(SCENE_SOURCES:assets/scenes/%.blend=build/assets/scenes/%_exits.txt)
.PHONY: all_exits

###
# repair
###

REPAIR_SOURCES := $(shell find assets/repair -type f -name '*.blend' | sort)

REPAIRS := $(REPAIR_SOURCES:assets/repair/%.blend=filesystem/repair/%.repair)

filesystem/repair/%.repair: assets/repair/%.blend build/assets/scripts/globals.json $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	@mkdir -p $(dir $(@:filesystem/repair/%.repair=build/assets/repair/%.repair))
	echo $@ $<
	$(BLENDER_4) $< --background --factory-startup --python-exit-code 1 --python tools/mesh_export/repair.py -- $(@:filesystem/repair/%.repair=build/assets/repair/%.repair)
	$(MK_ASSET) -o $(dir $@) -w 256 $(@:filesystem/repair/%.repair=build/assets/repair/%.repair)

###
# tests
###

build/test_result.txt: $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	python3 tools/mesh_export/cutscene_test.py
	echo "success" > $@

build/blender_results.txt: $(EXPORT_SOURCE)
	@mkdir -p $(dir $@)
	$(BLENDER_4) --background --factory-startup --python-exit-code 1 --python tools/mesh_export/tests.py
	echo "success" > $@

###
# source code
###

SOURCES := $(shell find src/ ! -path 'src/hle/*' ! -name '*_test.c' ! -name 'main.c' ! -name 'inventory_mapping.c' -type f -name '*.c' | sort) src/player/inventory_mapping.c
SOURCE_OBJS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
OBJS := $(BUILD_DIR)/main.o $(SOURCE_OBJS)

###
# HLE build (HLE=1): junkrunner64-hle.z64 for high-level-emulation emulators (Delta,
# Mupen64Plus with GLideN64 + rsp-hle). It does not run on real hardware or accurate
# emulators. src/hle/ is Pandemonium's HLE backend (github.com/cbyeh/Pandemonium, hle/).
###

ifeq ($(HLE),1)
# CPU replacement of the RSP (src/hle/rspq_hle.c replaces libdragon's rspq.o at link time)
HLE_SOURCES := $(wildcard src/hle/*.c) $(wildcard src/hle/*.S)
OBJS += $(patsubst src/%,$(BUILD_DIR)/%.o,$(basename $(HLE_SOURCES)))
N64_ASFLAGS += -Isrc/hle/internal
N64_C_AND_CXX_FLAGS += $(HLE_EXTRA_CFLAGS) -DHLE_BUILD -Isrc/hle -Isrc/hle/internal -Isrc/hle/internal/rspq -I$(T3D_INST)/src/t3d
N64_LDFLAGS += --wrap sys_hw_memset --wrap sys_hw_memset16 --wrap sys_hw_memset32 --wrap sys_hw_memset64
# Z clears without a mid-frame SYNC_FULL (src/hle/clearz_hle.c)
N64_LDFLAGS += --wrap __rdpq_clear_z $(HLE_EXTRA_LDFLAGS)
# Models register their vertices with the converted-vertex cache (src/hle/ovl_t3d.c)
N64_LDFLAGS += --wrap t3d_model_load --wrap t3d_model_free
# libdragon IPL3 patched to boot on emulators without RDRAM chip emulation (src/hle/ipl3/)
junkrunner64-hle.z64: N64_ROM_HEADER=src/hle/ipl3/ipl3_hle.z64
junkrunner64-hle.z64: N64_ROM_TITLE="Junk Runner 64 HLE"
# Region-free ROMs have country code 0, which Delta treats as PAL (50 Hz pacing): declare NTSC
junkrunner64-hle.z64: N64_ROM_REGIONFREE=
# The backend is hot code: optimize it even though the game builds with -Og (last flag wins)
$(BUILD_DIR)/hle/%.o: CFLAGS += -O2 -std=gnu2x
junkrunner64-hle.z64: $(BUILD_DIR)/junkrunner64.dfs
# Delta's 2019 core halts on libdragon's EMUX probe opcodes: patch them out after linking
junkrunner64-hle.z64: $(BUILD_DIR)/junkrunner64-hle.elf.emux
$(BUILD_DIR)/junkrunner64-hle.elf.emux: $(BUILD_DIR)/junkrunner64-hle.elf
	python3 src/hle/tools/patch_emux.py $<
	@touch $@
$(BUILD_DIR)/junkrunner64-hle.elf: $(OBJS)
endif

TEST_SOURCES := $(shell find src/ -type f -name '*_test.c' | sort)
TEST_SOURCE_OBJS := $(TEST_SOURCES:src/%.c=$(BUILD_DIR)/%.o)
TEST_OBJS := $(SOURCE_OBJS) $(TEST_SOURCE_OBJS)

filesystem/: $(SPRITES) $(TMESHES) $(MATERIALS) $(SCENES) $(REPAIRS) $(FONTS) $(SCRIPTS_COMPILED) $(SOUND_EFFECTS) $(MUSIC) filesystem/scripts/globals.dat

$(BUILD_DIR)/junkrunner64.dfs: filesystem/ $(SPRITES) $(TMESHES) $(MATERIALS) $(SCENES) $(REPAIRS) $(FONTS) $(SCRIPTS_COMPILED) $(SOUND_EFFECTS) $(MUSIC) filesystem/scripts/globals.dat
$(BUILD_DIR)/junkrunner64.elf: $(OBJS)
$(BUILD_DIR)/junkrunner64_test.elf: $(TEST_OBJS)

build/%.asm: build/%.o
	mips-linux-gnu-objdump -S --disassemble $< > $@

junkrunner64.z64: N64_ROM_TITLE="SpellCraft"
junkrunner64.z64: $(BUILD_DIR)/junkrunner64.dfs

junkrunner64_test.z64: N64_ROM_TITLE="SpellCraft Test"
junkrunner64_test.z64: $(BUILD_DIR)/junkrunner64.dfs

clean:
	rm -rf $(BUILD_DIR)/* filesystem/ *.z64
.PHONY: clean

clean-fs:
	rm -rf filesystem/ $(BUILD_DIR)/junkrunner64.dfs
.PHONY: clean-resource

clean-src:
	find build -name "*.o" -type f -delete
	find build -name "*.d" -type f -delete

check-pairings:
	node tools/pairing_checker.js $(SOURCES) src/main.c
.PHONY: check-pairings

tools/mesh_export.zip: tools/mesh_export/__init__.py
	cd tools; zip -r mesh_export.zip mesh_export/

build/strings.txt: tools/mesh_export/export_text.py $(SCRIPTS)
	$(BLENDER_4) --background --factory-startup --python-exit-code 1 --python tools/mesh_export/export_text.py -- $@ $(SCRIPTS)

DEPENDENCY_FILES := $(wildcard $(BUILD_DIR)/**/*.d) $(wildcard $(BUILD_DIR)/*.d)
-include $(DEPENDENCY_FILES)