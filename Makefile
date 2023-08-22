all: mkfs server createLib

mkfs: mkfs.c ufs.h
	gcc mkfs.c -o mkfs

server: server.c ufs.h udp.h message.h udp.c
	gcc server.c udp.c -o server

createLib: mfs.h udp.h message.h mfs.c udp.c
	gcc -fPIC -g -c -Wall mfs.c
	gcc -fPIC -g -c -Wall udp.c
	gcc -shared -Wl,-soname,libmfs.so -o libmfs.so mfs.o udp.o -lc 

clean: 
	rm -f *.o server mkfs libmfs.so
