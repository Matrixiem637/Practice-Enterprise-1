#pragma once

// Gedeclareerd door de linker via EMBED_FILES in CMakeLists.txt.
// De inhoud van index.html wordt als binaire array in flash opgeslagen.

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");