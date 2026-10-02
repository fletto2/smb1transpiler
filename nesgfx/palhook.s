; nesgfx/palhook.s -- NES palette RAM emulation for the a2vera SMB1 port.
;
; The port replaces UpdateScreen with its own VRAM-buffer parser in the LC
; ($F576).  Its second pass keeps only nametable writes ($2000-$27FF) and
; drops everything else, so the game's palette writes ($3Fxx) never reach
; VERA.  nesgfx retargets that pass's "hi >= $28" branch at a trampoline in
; the parser's NOP pad ($F632: JMP PALHOOK), and this routine turns each
; $3Fxx write into a VERA palette write.  It always leaves through $F6E7,
; the parser's own "advance to the next buffer entry" path.
;
;   NES $3F00 / $3F10   -> VERA palette entry 0 (the backdrop)
;   NES $3F01-$3F0F     -> VERA offset 4+p, entries 1-3   (BG palettes)
;   NES $3F11-$3F1F     -> VERA offset 8+p, entries 1-3   (sprite palettes)
;   NES $3F04/08/0C/14/18/1C are never displayed and are skipped.
;
; ADRLO/ADRHI (32 each) and COLGB/COLR (64 each) are filled in by nesgfx.c.
; Assemble: ca65 palhook.s && ld65 -t none -S 0xB400 -o palhook.bin palhook.o
; The bytes are carried in nesgfx.c, so building needs no assembler.

VERA_LO   = $C400
VERA_MID  = $C401
VERA_HI   = $C402
VERA_DAT  = $C403
BUF_HI    = $F529       ; parser's copy of the entry's PPU address high byte
BUF_LO    = $F52A
BUF_FLAGS = $F52B       ; bit 6: repeat one byte
BUF_LEN   = $F52C       ; count, flags masked off
NEXT_ENT  = $F6E7       ; parser: step ($00) past this entry, loop

        .org $B400
palhook:
        lda BUF_HI
        cmp #$3F
        bne done
        lda BUF_LEN
        beq done
        sta cnt
        lda BUF_LO
        and #$1F
        sta idx
        ldy #$03
loop:   ldx idx
        lda adrhi,x
        beq skip
        sta VERA_MID
        lda adrlo,x
        sta VERA_LO
        lda #$11                ; bank 1, increment 1
        sta VERA_HI
        lda ($00),y
        and #$3F
        tax
        lda colgb,x
        sta VERA_DAT
        lda colr,x
        sta VERA_DAT
skip:   bit BUF_FLAGS
        bvs norep
        iny
norep:  ldx idx
        inx
        txa
        and #$1F
        sta idx
        dec cnt
        bne loop
done:   jmp NEXT_ENT
idx:    .byte 0
cnt:    .byte 0
adrlo:  .res 32
adrhi:  .res 32
colgb:  .res 64
colr:   .res 64
