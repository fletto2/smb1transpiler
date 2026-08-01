# smb1transpiler -- derive a2vera's disk from YOUR OWN SMB1 + SMAS ROMs.
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
SRC     = smb1transpiler.c sprites.c disk.c
DEPS    = smb1transpiler.h a2vera_blobs.h

smb1transpiler: $(SRC) $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRC)

# Put your own dumps in this folder first; nothing Nintendo ships in this repo.
run: smb1transpiler
	./smb1transpiler

# Fidelity report against a reference tileset.  Needs a checkout of the main
# a2vera tree, which is not part of this repo, so say so rather than forming a
# nonsense path out of an unset variable.
verify: smb1transpiler
	@test -n "$(A2VERA)" || { echo "set A2VERA=/path/to/a2vera first"; exit 1; }
	./smb1transpiler --verify $(A2VERA)/tools/tiles_vera.bin

clean:
	rm -f smb1transpiler
	rm -rf out

.PHONY: run verify clean
