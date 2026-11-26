/**
 * @file main.cpp
 * @brief Ponto de entrada único do firmware Elo5System (seleciona NODE_PORTA ou NODE_RACK).
 */

#include <Arduino.h>
#include "node_config.h"

#if defined(NODE_PORTA)
void setupPortaController();
void loopPortaController();
#elif defined(NODE_RACK)
void setupRackController();
void loopRackController();
#endif

void setup() {
#if defined(NODE_PORTA)
  setupPortaController();
#elif defined(NODE_RACK)
  setupRackController();
#endif
}

void loop() {
#if defined(NODE_PORTA)
  loopPortaController();
#elif defined(NODE_RACK)
  loopRackController();
#endif
}
