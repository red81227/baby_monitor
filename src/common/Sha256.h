#pragma once

#include <string>
#include <string_view>

// SHA-256，回傳小寫十六進位字串 (64 字元)
std::string sha256Hex(std::string_view data);
