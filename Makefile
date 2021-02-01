all: zx48k_libretro.so

zx48k_libretro.so: src/main.o
	gcc -shared -o $@ $+

src/main.o: src/main.c
	gcc -O0 -g -fPIC -o $@ -c $<

clean:
	rm -f zx48k_libretro src/main.o
