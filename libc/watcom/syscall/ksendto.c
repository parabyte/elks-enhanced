/****************************************************************************
*
* Description:  ELKS ksendto() system call.
*
****************************************************************************/

#include <sys/socket.h>
#include "watcom/syselks.h"

int ksendto(int __sock, const void *__message, size_t __length,
	struct socket_sendto_args *__args)
{
	sys_setseg(__message ? (void *)__message : __args);
	{
		syscall_res res = sys_call4(SYS_ksendto, __sock, (unsigned)__message,
			__length, (unsigned)__args);
		__syscall_return(int, res);
	}
}
