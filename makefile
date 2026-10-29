# HANEDIT - Open Watcom wmake makefile
#   wmake            -> HANEDIT.EXE (DOS/32A extender bound, single file)
CC     = wcc386
CFLAGS = -q -bt=dos -mf -ox -s -wx -zq -i=src -i=libhangul
HFLAGS = -q -bt=dos -mf -ox -s -zq -za99 -DENABLE_EXTERNAL_KEYBOARDS=0 -i=libhangul

FONTOBJS = font.obj fontd0.obj fontd1.obj fontd2.obj fontd3.obj fontd4.obj fonta.obj fontad0.obj fontad1.obj
OBJS = main.obj vga.obj kbd.obj $(FONTOBJS) hangulctype.obj hangulinputcontext.obj hangulkeyboard.obj

HANEDIT.EXE: $(OBJS)
	wlink name HANEDIT.EXE system dos32a option quiet option stack=64k option stub=$(%WATCOM)/binw/dos32a.exe file { $(OBJS) }

main.obj: src/main.c src/vga.h src/kbd.h src/font.h libhangul/hangul.h
	$(CC) $(CFLAGS) src/main.c -fo=$@
vga.obj: src/vga.c src/vga.h src/font.h
	$(CC) $(CFLAGS) src/vga.c -fo=$@
kbd.obj: src/kbd.c src/kbd.h
	$(CC) $(CFLAGS) src/kbd.c -fo=$@
font.obj: src/font.c src/font.h
	$(CC) $(CFLAGS) src/font.c -fo=$@
fontd0.obj: src/fontd0.c
	$(CC) $(CFLAGS) src/fontd0.c -fo=$@
fontd1.obj: src/fontd1.c
	$(CC) $(CFLAGS) src/fontd1.c -fo=$@
fontd2.obj: src/fontd2.c
	$(CC) $(CFLAGS) src/fontd2.c -fo=$@
fontd3.obj: src/fontd3.c
	$(CC) $(CFLAGS) src/fontd3.c -fo=$@
fontd4.obj: src/fontd4.c
	$(CC) $(CFLAGS) src/fontd4.c -fo=$@
fonta.obj: src/fonta.c src/font.h
	$(CC) $(CFLAGS) src/fonta.c -fo=$@
fontad0.obj: src/fontad0.c
	$(CC) $(CFLAGS) src/fontad0.c -fo=$@
fontad1.obj: src/fontad1.c
	$(CC) $(CFLAGS) src/fontad1.c -fo=$@
hangulctype.obj: libhangul/hangulctype.c
	$(CC) $(HFLAGS) libhangul/hangulctype.c -fo=$@
hangulinputcontext.obj: libhangul/hangulinputcontext.c
	$(CC) $(HFLAGS) libhangul/hangulinputcontext.c -fo=$@
hangulkeyboard.obj: libhangul/hangulkeyboard.c libhangul/hangulkeyboard.h
	$(CC) $(HFLAGS) libhangul/hangulkeyboard.c -fo=$@

clean: .SYMBOLIC
	rm -f *.obj HANEDIT.EXE *.err
