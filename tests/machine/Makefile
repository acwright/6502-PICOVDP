# Phase 14's programs for the AC6502 (PLAN.md Phase 14, docs/results/phase-14.md).
# Each is a .prg that BASIC's LOAD takes over XMODEM; tools/acectl.mjs sends
# them and reads their results. Needs cc65 (`brew install cc65`).
#
#   probe.prg       SPEC.md §16's DetectVdp and DetectFont, 10,000 times each
#   bus.prg         a 6502's tightest accesses, both port pairs
#   vdp-modes.prg   6502-EMULATOR's VdpModes.crt source, run from RAM (cart.inc)
#   vdp-layers.prg  6502-EMULATOR's VdpLayers.crt source, run from RAM
#   graphics-1.prg  6502-DOCS v1's samples/assembly/graphics-1.asm, as it stands
#
# Beside each, a .lbl of its labels, which is how acectl finds RESULTS.

REPO     := $(abspath ../..)
OUT      := $(REPO)/build/machine
EMULATOR ?= $(abspath $(REPO)/../../NodeJS/6502-EMULATOR)
DOCS     ?= $(abspath $(REPO)/../../NodeJS/6502-DOCS)
ASM      ?= $(abspath $(REPO)/../../Assembly/6502-ASM)

PRGS := probe bus vdp-modes vdp-layers graphics-1

.PHONY: all clean
all: $(PRGS:%=$(OUT)/%.prg)

$(OUT):
	mkdir -p $@

$(OUT)/probe.prg: probe.asm machine.inc prg.cfg | $(OUT)
	cl65 -t none -C prg.cfg -g -Ln $(OUT)/probe.lbl -l $(OUT)/probe.lst -o $@ probe.asm

$(OUT)/bus.prg: bus.asm machine.inc prg.cfg | $(OUT)
	cl65 -t none -C prg.cfg -g -Ln $(OUT)/bus.lbl -l $(OUT)/bus.lst -o $@ bus.asm

$(OUT)/vdp-modes.prg: vdp-modes.asm cart.inc prg.cfg $(EMULATOR)/samples/vdp-modes/VdpModes.asm | $(OUT)
	cl65 -t none -C prg.cfg --asm-include-dir $(EMULATOR)/samples/vdp-modes -g -Ln $(OUT)/vdp-modes.lbl -l $(OUT)/vdp-modes.lst -o $@ vdp-modes.asm

$(OUT)/vdp-layers.prg: vdp-layers.asm cart.inc prg.cfg $(EMULATOR)/samples/vdp-layers/VdpLayers.asm | $(OUT)
	cl65 -t none -C prg.cfg --asm-include-dir $(EMULATOR)/samples/vdp-layers -g -Ln $(OUT)/vdp-layers.lbl -l $(OUT)/vdp-layers.lst -o $@ vdp-layers.asm

# The docs' sample from the frozen v1 edition, where it still lives, built
# against the legacy 6502.inc it was written for. BIOS 2.0 keeps every entry
# point it calls at the same address.
$(OUT)/graphics-1.asm: | $(OUT)
	git -C $(DOCS) show v1:samples/assembly/graphics-1.asm > $@

$(OUT)/graphics-1.prg: $(OUT)/graphics-1.asm prg.cfg
	cl65 -t none -C prg.cfg --asm-include-dir $(ASM) -g -Ln $(OUT)/graphics-1.lbl -l $(OUT)/graphics-1.lst -o $@ $(OUT)/graphics-1.asm

clean:
	rm -rf $(OUT)
