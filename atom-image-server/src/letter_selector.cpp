#include "letter_selector.h"
#include <M5Unified.h>

static char selectedLetter = 'A';

void initLetterSelector() {
  selectedLetter = 'A';
}

void nextLetter() {
  if (selectedLetter >= 'Z') {
    selectedLetter = 'A';
  } else {
    selectedLetter++;
  }
}

char getSelectedLetter() {
  return selectedLetter;
}

void showSelectedLetter() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.setTextSize(6);

  M5.Display.setCursor(45, 35);
  M5.Display.printf("%c", selectedLetter);
}