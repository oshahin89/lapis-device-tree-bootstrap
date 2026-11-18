#ifndef LAPIS_UFSINIT_H
#define LAPIS_UFSINIT_H

/* Bring the UFS host up and initialise the device so the SWEET xnu driver
 * can adopt it. Returns 0 on success, or a reason string. */
const char *ufs_bring_up(void);

#endif
