# Dreamcast calibration ELF (needs: source $KOS_BASE/environ.sh)
# Run: kos-tool -t <dc-ip> -x calib.elf > calib.log
calib.elf: calib.c calib.S
	kos-cc -O2 -o $@ calib.c calib.S
clean:
	rm -f calib.elf
