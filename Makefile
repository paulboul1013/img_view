CFLAGS = -Wextra 
main:
	gcc $(CFLAGS) -o main main.c -lSDL2

clean:
	rm -f main