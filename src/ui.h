#ifndef UI_H
#define UI_H

#include <stddef.h>

/* ANSI colour helpers */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_DIM     "\033[2m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_BLUE    "\033[34m"
#define COL_MAGENTA "\033[35m"
#define COL_CYAN    "\033[36m"
#define COL_WHITE   "\033[37m"

/* Symbols */
#define SYM_OK      COL_GREEN  "[  OK  ]" COL_RESET
#define SYM_WARN    COL_YELLOW "[ WARN ]" COL_RESET
#define SYM_FAIL    COL_RED    "[ FAIL ]" COL_RESET
#define SYM_NEW     COL_CYAN   "[ NEW  ]" COL_RESET
#define SYM_MOD     COL_YELLOW "[ MOD  ]" COL_RESET
#define SYM_INFO    COL_BLUE   "[ INFO ]" COL_RESET

void ui_clear_screen(void);
void ui_print_banner(void);
void ui_print_menu(void);

/* Ask a yes/no question; returns 1 for yes, 0 for no */
int  ui_ask_yn(const char *question);

/* Ask with custom prompt and choices; returns selected char (lowercased) */
char ui_ask_choice(const char *prompt, const char *choices);

/* Progress bar: call with current and total; clears when current==total */
void ui_progress(size_t current, size_t total, const char *label);

#endif /* UI_H */
