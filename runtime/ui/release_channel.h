#pragma once
#include <string>

// Historical tags remain readable; new releases use multi-beta<N>.<revision>.
static inline size_t multi_tag_prefix(const std::string &tag) {
    if (tag.rfind("multi-beta", 0) == 0) return 10;
    if (tag.rfind("enhanced-beta", 0) == 0) return 13;
    return 0;
}

static inline long long multi_tag_rank(const std::string &tag) {
    size_t at = multi_tag_prefix(tag);
    if (!at) return -1;
    auto number = [&](unsigned limit) -> long long {
        size_t start = at;
        unsigned value = 0;
        while (at < tag.size() && tag[at] >= '0' && tag[at] <= '9') {
            unsigned digit = tag[at++] - '0';
            if (value > (limit - digit) / 10) return -1;
            value = value * 10 + digit;
        }
        if (at == start || value == 0) return -1;
        return value;
    };
    long long beta = number(2147483647u);
    if (beta < 0 || at == tag.size() || tag[at++] != '.') return -1;
    long long revision = number(65535);
    return revision > 0 && at == tag.size() ? (beta << 16) + revision : -1;
}

static inline std::string multi_tag_label(const std::string &tag) {
    if (multi_tag_rank(tag) >= 0) return "Multi Beta " + tag.substr(multi_tag_prefix(tag));
    if (tag.rfind("test-", 0) == 0) return "Test build " + tag.substr(5);
    return tag;
}
