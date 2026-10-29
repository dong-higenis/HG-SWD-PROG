#ifndef PROG_KEYBOARD_H
#define PROG_KEYBOARD_H
#include <stdbool.h>
#include <stdint.h>

/* Text entry with four buttons: UP/DN turn a character wheel, SEL adds the character, M deletes the last one,
 * and holding SEL opens a menu (done, jump to a character group, show/hide, clear, cancel). */
#define KBD_TEXT_MAX 64

typedef enum
{
  KBD_KEY_UP,
  KBD_KEY_DN,
  KBD_KEY_ADD,  /* SEL released before the hold time. */
  KBD_KEY_MENU, /* SEL held. */
  KBD_KEY_DEL   /* M. */
} kbd_key_t;

typedef enum
{
  KBD_EDIT,
  KBD_DONE,
  KBD_CANCEL
} kbd_result_t;

typedef struct
{
  char    text[KBD_TEXT_MAX + 1];
  uint8_t len;
  uint8_t max;
  uint8_t pick; /* Wheel position in the character set. */
  bool    menu;
  uint8_t menu_selected;
  bool    shown; /* Text visible; otherwise all but the last character are masked. */
} kbd_t;

void         kbdBegin(kbd_t *kbd, uint8_t max, const char *initial, bool shown);
kbd_result_t kbdKey(kbd_t *kbd, kbd_key_t key);
char         kbdChar(const kbd_t *kbd, int offset); /* Wheel character offset from the current one. */
const char  *kbdGroup(const kbd_t *kbd);              /* "abc", "ABC", "123" or "#+=". */
unsigned     kbdMenuCount(void);
const char  *kbdMenuItem(const kbd_t *kbd, unsigned item);
#endif
