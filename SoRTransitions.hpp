#pragma once

#include "data_types.hpp"

/**
 * @file SoRTransitions.hpp
 * @brief Custom screen transition functions for Streets of Rage
 */

class StreetsOfRage;

// Custom transition functions
void StreetsOfRage::clear_vram_and_fade_out();
void StreetsOfRage::fade_in_from_black();
