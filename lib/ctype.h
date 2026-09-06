#ifndef LIB_CTYPE_H
#define LIB_CTYPE_H
// should use a byte table
inline int islower(int c) { return c >= 'a' && c <= 'z'; }
inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
inline int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline int isdigit(int c) { return c >= '0' && c <= '9'; }
inline int isxdigit(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
inline int isalnum(int c) { return isdigit(c) || isalpha(c); }
inline int isblank(int c) { return c == ' ' || c == '\t'; }
inline int isspace(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
inline int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
#endif
