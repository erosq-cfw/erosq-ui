/* Inject one key press+release to an evdev node. Usage: keyinject <dev> <code>.
 * 16-byte event record matches the 32-bit-MIPS kernel input_event_size(). */
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
struct ie { unsigned int sec, usec; unsigned short type, code; int value; };
static int emit(int fd, unsigned short t, unsigned short c, int v){
    struct ie e; memset(&e,0,sizeof e); e.type=t; e.code=c; e.value=v;
    return write(fd,&e,sizeof e)==(long)sizeof e?0:-1;
}
int main(int argc,char**argv){
    if(argc<3) return 2;
    int fd=open(argv[1],O_WRONLY); if(fd<0) return 1;
    int code=atoi(argv[2]);
    emit(fd,1,code,1); emit(fd,0,0,0); usleep(60000);   /* EV_KEY press + SYN */
    emit(fd,1,code,0); emit(fd,0,0,0);                  /* EV_KEY release + SYN */
    return 0;
}
