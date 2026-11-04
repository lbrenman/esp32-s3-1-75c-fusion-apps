# UI preview: runs the real UI code (main/ui, main/apps) on your Mac against LVGL 9.5,
# clicks/swipes through the screens, and saves screenshots to shots/.
#
#   cd tools/ui_preview
#   make                # 1.75C (round 466x466): first run clones and builds LVGL (~1 min), writes shots/*.png
#   make BOARD=2_06     # 2.06 (410x502, rounded corners): writes shots_2_06/*.png
#   make clean          # remove the build (keeps the LVGL clone)
#
# Needs: a C compiler (Xcode command line tools), git, python3. PNGs need Pillow
# (pip3 install pillow); without it you get .ppm files, which Preview also opens.

BOARD    ?= 1_75c
LVGL_TAG := v9.5.0
LVGL     := lvgl
MAIN     := ../../main
CFLAGS   := -O1 -g -I. -Istub -I$(LVGL) -DLV_CONF_INCLUDE_SIMPLE -Wno-unused-parameter
BOARD_INC := ../../boards/$(BOARD)/board_bsp/include
SHOTS    := $(if $(filter 1_75c,$(BOARD)),shots,shots_$(BOARD))
SIM      := sim_$(BOARD)
UI_SRCS  := $(MAIN)/ui/ui_theme.c $(MAIN)/ui/ui_nav.c $(MAIN)/ui/ui_widgets.c \
            $(wildcard $(MAIN)/ui/ui_icons_*.c) \
            $(MAIN)/apps/apps.c $(MAIN)/apps/app_home.c $(MAIN)/apps/app_settings.c $(MAIN)/apps/app_voice.c \
            $(MAIN)/apps/app_stocks.c $(MAIN)/apps/app_translate.c $(MAIN)/apps/translate_langs.c \
            $(wildcard $(MAIN)/ui/ui_font_*.c) $(MAIN)/text_ascii.c

.PHONY: all shots clean
all: shots

$(LVGL):
	git clone --depth 1 --branch $(LVGL_TAG) https://github.com/lvgl/lvgl.git $(LVGL)

liblvgl.a: | $(LVGL)
	@mkdir -p obj
	@echo "Building LVGL (one time)..."
	@find $(LVGL)/src -name '*.c' | while read f; do \
	    o=obj/$$(echo $$f | tr '/' '_' | sed 's/\.c$$/.o/'); \
	    echo "$(CC) $(CFLAGS) -c $$f -o $$o"; \
	done | xargs -P 8 -I{} sh -c '{}'
	ar rcs $@ obj/*.o

$(SIM): liblvgl.a sim.c stubs.c $(UI_SRCS) $(wildcard $(MAIN)/ui/*.h $(MAIN)/apps/*.h $(MAIN)/*.h $(BOARD_INC)/*.h)
	$(CC) $(CFLAGS) -Wall -Wno-missing-field-initializers -I$(MAIN) -I$(MAIN)/ui -I$(MAIN)/apps -I$(BOARD_INC) \
	    -DAPP_BOARD_$(shell echo $(BOARD) | tr a-z A-Z)=1 -o $@ sim.c stubs.c $(UI_SRCS) liblvgl.a -lm

shots: $(SIM)
	@rm -rf $(SHOTS) && mkdir -p $(SHOTS)
	./$(SIM) $(SHOTS) | grep -v "^\[Warn\]"
	@python3 to_png.py $(SHOTS) || echo "(Pillow not installed: screenshots left as .ppm)"

clean:
	rm -rf obj liblvgl.a sim sim_* shots shots_*
