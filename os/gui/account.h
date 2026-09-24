/*
 * OmniOS — os/gui/account.h
 *
 * The desktop's user account: its name and password, for the sign-in /
 * lock screen (shell) and Settings > Accounts. Passwords live in
 * /etc/shadow as crypt(3) SHA-512 hashes ("$6$"); an empty or missing
 * entry means the account has no password. OMNI_SHADOW overrides the
 * file (tests).
 */
#ifndef OMNI_OS_GUI_ACCOUNT_H
#define OMNI_OS_GUI_ACCOUNT_H

#define OMNI_SHADOW_FILE "/etc/shadow"

/* login name and display name (GECOS full name, else the login name) */
const char *omni_account_login(void);
const char *omni_account_display(void);

/* 1 if the account has a password */
int omni_account_has_password(void);
/* 1 if `pw` is right (always 1 when there is no password) */
int omni_account_check(const char *pw);
/* set the password ("" or NULL removes it); 0 on success, -1 on error */
int omni_account_set_password(const char *pw);

#endif /* OMNI_OS_GUI_ACCOUNT_H */
