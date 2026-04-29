#pragma once

#include <cstdint>

#define BUILD_ID(node)                                                                                  \
int j = 0;                                                                                              \
for (int i = 0; i < node->id_size; ++i) {                                                               \
    Key cur = node->left + (i + 1) * (static_cast<int64_t>(node->right) - node->left) / node->id_size;  \
    while (j < node->rep_size && node->rep[j] < cur) {                                                  \
        ++j;                                                                                            \
    }                                                                                                   \
    node->id[i] = j;                                                                                    \
}                                                                                                       \

#define ID_LOCATE(node) \
int j = (static_cast<int64_t>(key) - node->left) * node->id_size / (static_cast<int64_t>(node->right) - node->left);  \
l = j == 0 ? -1 : node->id[j - 1] - 1;                                                                                \
r = node->id[j];                                                                                                      \
