#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define BAR_WIDTH 40

/* If fgets didn't consume the whole line (input longer than the buffer),
 * drain the rest so it doesn't leak into the next prompt. */
static void drain_line(const char *buf)
{
    if (strchr(buf, '\n')) return;
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

void ui_clear_screen(void)
{
    printf("\033[2J\033[H");
    fflush(stdout);
}

void ui_print_banner(void)
{
    printf("\n");
    printf(COL_CYAN COL_BOLD
           "  ███████╗██╗   ██╗███████╗██╗  ██╗ █████╗ ███████╗██╗  ██╗\n"
           "  ██╔════╝╚██╗ ██╔╝██╔════╝██║  ██║██╔══██╗██╔════╝██║  ██║\n"
           "  ███████╗ ╚████╔╝ ███████╗███████║███████║███████╗███████║\n"
           "  ╚════██║  ╚██╔╝  ╚════██║██╔══██║██╔══██║╚════██║██╔══██║\n"
           "  ███████║   ██║   ███████║██║  ██║██║  ██║███████║██║  ██║\n"
           "  ╚══════╝   ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝\n"
           COL_RESET);
    printf(COL_DIM
           "  File Integrity Monitor  ·  SHA3-512  ·  v" VERSION "\n"
           COL_RESET);
    printf("\n");
    printf(COL_DIM "  ─────────────────────────────────────────────────────────────\n" COL_RESET);
}

void ui_print_menu(void)
{
    printf("\n");
    printf(COL_BOLD "  What would you like to do?\n\n" COL_RESET);
    printf("  " COL_CYAN "1" COL_RESET "  →  Create / Rebuild database\n");
    printf("  " COL_CYAN "2" COL_RESET "  →  Verify integrity\n");
    printf("  " COL_CYAN "3" COL_RESET "  →  About\n");
    printf("  " COL_CYAN "4" COL_RESET "  →  Exit\n");
    printf("\n");
    printf(COL_DIM "  ─────────────────────────────────────────────────────────────\n" COL_RESET);
    printf("\n");
}

void ui_print_about(void)
{
    ui_clear_screen();
    ui_print_banner();
    printf("\n");
    printf("  " COL_BOLD "About Syshash\n" COL_RESET);
    printf("\n");
    printf("  " COL_DIM "File Integrity Monitor using SHA3-512 hashing.\n" COL_RESET);
    printf("  " COL_DIM "Version " VERSION "\n" COL_RESET);
    printf("\n");
    printf("  " COL_BOLD "Author : " COL_RESET COL_CYAN AUTHOR COL_RESET "\n");
    printf("\n");
    printf(COL_DIM "  ─────────────────────────────────────────────────────────────\n" COL_RESET);
    printf("\n");
}

int ui_ask_yn(const char *question)
{
    char buf[8];
    for (;;) {
        printf("  %s " COL_DIM "[y/n]" COL_RESET " ", question);
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) return 0;
        drain_line(buf);
        char c = tolower((unsigned char)buf[0]);
        if (c == 'y') return 1;
        if (c == 'n') return 0;
        printf("  " COL_YELLOW "Please answer y or n.\n" COL_RESET);
    }
}

char ui_ask_choice(const char *prompt, const char *choices)
{
    char buf[8];
    for (;;) {
        printf("  %s " COL_DIM "[%s]" COL_RESET ": ", prompt, choices);
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) continue;
        drain_line(buf);
        char c = tolower((unsigned char)buf[0]);
        if (strchr(choices, c)) return c;
        printf("  " COL_YELLOW "Invalid choice. Options: %s\n" COL_RESET, choices);
    }
}

void ui_progress(size_t current, size_t total, const char *label)
{
    if (total == 0) return;

    /* Files can appear between the pre-count and the hashing pass, so clamp
     * to avoid drawing a bar wider than BAR_WIDTH. */
    if (current > total) current = total;

    int filled = (int)((double)current / (double)total * BAR_WIDTH);
    int empty  = BAR_WIDTH - filled;

    printf("\r  " COL_CYAN);
    printf("[");
    int i;
    for (i = 0; i < filled; i++) printf("█");
    printf(COL_DIM);
    for (i = 0; i < empty;  i++) printf("░");
    printf(COL_RESET COL_CYAN "]" COL_RESET);
    printf("  %zu / %zu  " COL_DIM "%s" COL_RESET, current, total, label);
    printf("\033[K");   /* clear to end of line so shorter labels don't leave residue */
    fflush(stdout);

    if (current == total)
        printf("\n");
}
