# Host/QEMU equivalence test: frame-less SH-4 IDCT (src/idct_sh4.S) against
# the vendored C reference. Needs gcc-sh4-linux-gnu, libc6-dev-sh4-cross, qemu-user.
CC      = sh4-linux-gnu-gcc
QEMU    ?= qemu-sh4
CASES   ?= 1000000
VENDOR  := ../../vendor/ffmpeg/libavcodec/simple_idct.c
B       := build

all: $(B)/idct_test
$(B)/ref_idct.c: $(VENDOR) | $(B)
	sed -n '1,/^\/\* 2x4x8 idct \*\//p' $< > $@
$(B)/ref_idct.o: $(B)/ref_idct.c | $(B)
	$(CC) -O2 -Ishim -c $< -o $@
$(B)/idct_sh4.o: ../../src/idct_sh4.S
	$(CC) -c $< -o $@
$(B)/test.o: test.c | $(B)
	$(CC) -O2 -c $< -o $@
$(B)/abi_check.o: abi_check.S | $(B)
	$(CC) -c $< -o $@
$(B)/idct_test: $(B)/test.o $(B)/ref_idct.o $(B)/idct_sh4.o $(B)/abi_check.o | $(B)
	$(CC) -static -o $@ $^
$(B):
	mkdir -p $@
run: $(B)/idct_test
	$(QEMU) $(B)/idct_test $(CASES)
clean:
	rm -rf $(B)
.PHONY: all run clean

# Dreamcast on-device equivalence ELF (needs: source $KOS_BASE/environ.sh).
# Run: kos-tool -t <dc-ip> -x build/idct_equiv.elf > log
dc: $(B)/ref_idct.c | $(B)
	kos-cc -O2 -DASM_UNDERSCORE -Ishim -o $(B)/idct_equiv.elf test.c $(B)/ref_idct.c ../../src/idct_sh4.S abi_check.S
.PHONY: dc

# MAC.W kernel (src/idct_sh4_mac.S): permuted-coefficient input
$(B)/idct_sh4_mac.o: ../../src/idct_sh4_mac.S | $(B)
	$(CC) -DMAC_EMU $(ASFLAGS_X) -c $< -o $@
$(B)/test_mac.o: test.c | $(B)
	$(CC) -O2 -DTEST_MAC -c $< -o $@
$(B)/idct_test_mac: $(B)/test_mac.o $(B)/ref_idct.o $(B)/idct_sh4_mac.o $(B)/abi_check.o | $(B)
	$(CC) -static -o $@ $^
run-mac: $(B)/idct_test_mac
	$(QEMU) $(B)/idct_test_mac $(CASES)
.PHONY: run-mac

# Dreamcast on-device equivalence ELF for the MAC.W kernel (real mac.w, no emulation)
dc-mac: $(B)/ref_idct.c | $(B)
	kos-cc -O2 -DASM_UNDERSCORE -DTEST_MAC -Ishim -o $(B)/idct_equiv_mac.elf test.c $(B)/ref_idct.c ../../src/idct_sh4_mac.S abi_check.S
.PHONY: dc-mac

# On-device IDCT micro-benchmark: C reference vs muls.w asm vs MAC.W asm
dc-bench: $(B)/ref_idct.c | $(B)
	kos-cc -O3 -DASM_UNDERSCORE -Ishim -o $(B)/idct_bench.elf bench.c $(B)/ref_idct.c ../../src/idct_sh4.S ../../src/idct_sh4_mac.S
.PHONY: dc-bench
