#ifndef __ACPBL_UDP_GMA_H__
#define __ACPBL_UDP_GMA_H__

/* Maximum Fragment Size */
#ifndef ACPBL_UDP_MFS
#define ACPBL_UDP_MFS 1440
#endif

/* Command Queue Size in binary exponent */
#ifndef ACPBL_UDP_CQ_SIZE
#define ACPBL_UDP_CQ_SIZE 12
#endif

/* Command Station Size in binary exponent */
#ifndef ACPBL_UDP_CS_SIZE
#define ACPBL_UDP_CS_SIZE 4
#endif

/* Delegate Station Size in binary exponent */
#ifndef ACPBL_UDP_DS_SIZE
#define ACPBL_UDP_DS_SIZE 20
#endif

/* Delegate Window Size in binary exponent */
#ifndef ACPBL_UDP_SW_SIZE
#define ACPBL_UDP_SW_SIZE 3
#endif

int iacpbludp_init_gma(void);
int iacpbludp_finalize_gma(void);
void iacpbludp_abort_gma(void);

#endif /* acpbl_udp_gma.h */