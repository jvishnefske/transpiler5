/* Minimal lwIP options for the transpile corpus (NOT a runtime port). Its only
 * job is to let clang build an AST for lwIP's src/core so emitrust-cc can be
 * measured against it; it disables the OS/socket layers that would pull in a
 * sys port. Adjust to widen the corpus (e.g. enable LWIP_SOCKET with a real
 * sys_arch). */
#ifndef LWIP_CORPUS_LWIPOPTS_H
#define LWIP_CORPUS_LWIPOPTS_H

#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_DHCP               1
#define LWIP_ICMP               1
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_IPV4               1
#define LWIP_IPV6               1
#define MEM_ALIGNMENT           4
#define LWIP_STATS              0

#endif /* LWIP_CORPUS_LWIPOPTS_H */
