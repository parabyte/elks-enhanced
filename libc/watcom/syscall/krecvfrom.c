/****************************************************************************
*
* Description:  ELKS krecvfrom() system call.
*
****************************************************************************/

#include <sys/socket.h>
#include "watcom/syselks.h"

int krecvfrom(int __sock, void *__buffer, size_t __length,
	struct socket_recvfrom_args *__args)
{
	sys_setseg(__buffer ? __buffer : __args);
	{
		syscall_res res = sys_call4(SYS_krecvfrom, __sock, (unsigned)__buffer,
			__length, (unsigned)__args);
		__syscall_return(int, res);
	}
}
