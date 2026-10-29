#include "prog_keyboard.h"
#include <string.h>

/* Groups follow one another on the wheel; the menu jumps to a group's first character. */
static const char charset[] = "abcdefghijklmnopqrstuvwxyz"
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                              "0123456789"
                              "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~ ";
#define CHARSET_SIZE (sizeof(charset) - 1)

static const struct
{
  const char *name;
  uint8_t     first;
} groups[] = {{"abc", 0}, {"ABC", 26}, {"123", 52}, {"#+=", 62}};

typedef enum
{
  MENU_DONE,
  MENU_LOWER,
  MENU_UPPER,
  MENU_DIGITS,
  MENU_SYMBOLS,
  MENU_SHOW,
  MENU_CLEAR,
  MENU_CANCEL,
  MENU_COUNT
} menu_t;

void kbdBegin(kbd_t *kbd, uint8_t max, const char *initial, bool shown)
{
  memset(kbd, 0, sizeof(*kbd));
  kbd->max = max > KBD_TEXT_MAX ? KBD_TEXT_MAX : max;
  if (initial)
  {
    strncpy(kbd->text, initial, kbd->max);
    kbd->len = (uint8_t)strlen(kbd->text);
  }
  kbd->shown = shown;
}

char kbdChar(const kbd_t *kbd, int offset)
{
  int i = ((int)kbd->pick + offset) % (int)CHARSET_SIZE;
  return charset[i < 0 ? i + (int)CHARSET_SIZE : i];
}

const char *kbdGroup(const kbd_t *kbd)
{
  unsigned group = 0;
  for (unsigned g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
  {
    if (kbd->pick >= groups[g].first)
    {
      group = g;
    }
  }
  return groups[group].name;
}

unsigned kbdMenuCount(void)
{
  return MENU_COUNT;
}

const char *kbdMenuItem(const kbd_t *kbd, unsigned item)
{
  static const char *names[MENU_COUNT] = {"Done", "abc", "ABC", "123", "#+=", "Show text", "Clear", "Cancel"};
  return item == MENU_SHOW && kbd->shown ? "Hide text" : item < MENU_COUNT ? names[item] : "";
}

static kbd_result_t kbdMenuKey(kbd_t *kbd, kbd_key_t key)
{
  switch (key)
  {
    case KBD_KEY_UP:
      kbd->menu_selected = (uint8_t)((kbd->menu_selected + MENU_COUNT - 1) % MENU_COUNT);
      break;
    case KBD_KEY_DN:
      kbd->menu_selected = (uint8_t)((kbd->menu_selected + 1) % MENU_COUNT);
      break;
    case KBD_KEY_DEL:
    case KBD_KEY_MENU:
      kbd->menu = false;
      break;
    case KBD_KEY_ADD:
      kbd->menu = false;
      switch ((menu_t)kbd->menu_selected)
      {
        case MENU_DONE:
          return KBD_DONE;
        case MENU_CANCEL:
          return KBD_CANCEL;
        case MENU_SHOW:
          kbd->shown = !kbd->shown;
          break;
        case MENU_CLEAR:
          kbd->len     = 0;
          kbd->text[0] = 0;
          break;
        default:
          kbd->pick = groups[kbd->menu_selected - MENU_LOWER].first;
          break;
      }
      break;
  }
  return KBD_EDIT;
}

kbd_result_t kbdKey(kbd_t *kbd, kbd_key_t key)
{
  if (kbd->menu)
  {
    return kbdMenuKey(kbd, key);
  }
  switch (key)
  {
    case KBD_KEY_UP:
      kbd->pick = (uint8_t)((kbd->pick + CHARSET_SIZE - 1) % CHARSET_SIZE);
      break;
    case KBD_KEY_DN:
      kbd->pick = (uint8_t)((kbd->pick + 1) % CHARSET_SIZE);
      break;
    case KBD_KEY_ADD:
      if (kbd->len < kbd->max)
      {
        kbd->text[kbd->len++] = charset[kbd->pick];
        kbd->text[kbd->len]   = 0;
      }
      break;
    case KBD_KEY_DEL:
      if (!kbd->len)
      {
        return KBD_CANCEL; /* M on an empty field leaves, like M everywhere else. */
      }
      kbd->text[--kbd->len] = 0;
      break;
    case KBD_KEY_MENU:
      kbd->menu          = true;
      kbd->menu_selected = MENU_DONE;
      break;
  }
  return KBD_EDIT;
}
