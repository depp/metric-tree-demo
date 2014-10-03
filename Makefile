CC = gcc
CFLAGS = -O3 -Wall -Wextra -Werror -std=gnu99

all : tree

clean :
	rm -f tree

tree : tree.o Makefile
	$(CC) tree.o -o tree
